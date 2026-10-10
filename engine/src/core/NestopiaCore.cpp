#include "core/NestopiaCore.h"

#include <cstring>
#include <sstream>

#include "core/api/NstApiCheats.hpp"
#include "core/api/NstApiEmulator.hpp"
#include "core/api/NstApiInput.hpp"
#include "core/api/NstApiMachine.hpp"
#include "core/api/NstApiSound.hpp"
#include "core/api/NstApiVideo.hpp"
#include "core/NstMachine.hpp"

#if !defined(RN_NESTOPIA_COMMIT) || !defined(RN_NESTOPIA_PATCHLEVEL)
#error "RN_NESTOPIA_COMMIT / RN_NESTOPIA_PATCHLEVEL must be defined by the build (cmake/NestopiaCore.cmake)"
#endif

namespace rn {

using namespace Nes::Api;

struct NestopiaCore::Impl {
  Emulator emu;
  Nes::Core::Input::Controllers controllers;
  Nes::Core::Video::Output videoOut;
  Nes::Core::Sound::Output soundOut;
};

std::string NestopiaCore::staticCompatId() {
  return std::string("nestopia-ue@") + std::string(RN_NESTOPIA_COMMIT).substr(0, 12) + "+p" +
         std::to_string(RN_NESTOPIA_PATCHLEVEL) + "+adapter" + std::to_string(kAdapterVersion) + "+ntsc";
}

std::string NestopiaCore::buildId() const {
  std::string b = std::string("nestopia-ue ") + RN_NESTOPIA_COMMIT + " patchlevel " +
                  std::to_string(RN_NESTOPIA_PATCHLEVEL);
#if defined(__clang__)
  b += " clang-" __clang_version__;
#elif defined(__GNUC__)
  b += " gcc-" __VERSION__;
#elif defined(_MSC_VER)
  b += " msvc-" + std::to_string(_MSC_VER);
#endif
  return b;
}

NestopiaCore::NestopiaCore()
    : impl_(new Impl()),
      video_(size_t(kVideoWidth) * kVideoHeight, 0xFF000000u),
      codes_(size_t(kVideoWidth) * kVideoHeight, 0x0F),
      audio_(kMaxSamplesPerFrame, 0) {}

NestopiaCore::~NestopiaCore() {
  if (loaded_) {
    Machine m(impl_->emu);
    m.Power(false);
    m.Unload();
  }
}

static Status nstErr(Err code, const char* what, Nes::Result r) {
  return Error(code, std::string(what) + " failed (Nestopia result " + std::to_string(int(r)) + ")");
}

Status NestopiaCore::loadROM(const uint8_t* data, size_t size) {
  if (!data || size < 16) return Error(Err::RomInvalid, "ROM too small");
  if (loaded_) {
    // Unload/Load on the same Nes::Api::Emulator does not reset every APU register (e.g. the
    // triangle's), so power-on would depend on history. Always start from a new instance.
    Machine old(impl_->emu);
    old.Power(false);
    old.Unload();
    loaded_ = false;
    impl_.reset(new Impl());
  }
  Emulator& emu = impl_->emu;
  Machine machine(emu);

  machine.SetRamPowerState(0);  // all-zero RAM at power-on; never the rand() variant
  std::string bytes(reinterpret_cast<const char*>(data), size);
  std::istringstream in(bytes, std::ios::in | std::ios::binary);
  Nes::Result r = machine.Load(in, Machine::FAVORED_NES_NTSC, Machine::DONT_ASK_PROFILE);
  if (NES_FAILED(r)) return nstErr(Err::RomInvalid, "ROM load", r);
  if (machine.Is(Machine::DISK) || machine.Is(Machine::SOUND))
    return Error(Err::RomInvalid, "only cartridge images (iNES/NES 2.0) are supported");
  r = machine.SetMode(Machine::NTSC);
  if (NES_FAILED(r)) return nstErr(Err::RomInvalid, "NTSC mode", r);

  // Video: plain 256x240, 32-bit BGRA, fixed palette.
  Video video(emu);
  Video::RenderState rs;
  rs.filter = Video::RenderState::FILTER_NONE;
  rs.width = kVideoWidth;
  rs.height = kVideoHeight;
  rs.bits.count = 32;
  rs.bits.mask.r = 0x00FF0000;
  rs.bits.mask.g = 0x0000FF00;
  rs.bits.mask.b = 0x000000FF;
  r = video.SetRenderState(rs);
  if (NES_FAILED(r)) return nstErr(Err::Internal, "video render state", r);
  video.SetBrightness(0);
  video.SetSaturation(0);
  video.SetContrast(0);
  video.SetHue(0);
  video.SetSharpness(0);
  video.SetDecoder(Video::Decoder(Video::DECODER_CANONICAL));
  video.GetPalette().SetMode(Video::Palette::MODE_YUV);
  video.EnableUnlimSprites(false);

  // Audio: 48 kHz, default mix, no speed adaption.
  Sound sound(emu);
  sound.SetSampleRate(kSampleRate);
  sound.SetSpeed(Sound::DEFAULT_SPEED);
  sound.SetVolume(Sound::ALL_CHANNELS, Sound::DEFAULT_VOLUME);
  sound.SetAutoTranspose(false);
  sound.SetFilter(false);  // float biquad state is not part of savestates; DC blocker (saved) stays
  sound.Mute(false);

  // Input: two standard pads.
  Input input(emu);
  input.ConnectAdapter(Input::ADAPTER_NES);
  input.ConnectController(0, Input::PAD1);
  input.ConnectController(1, Input::PAD2);
  input.ConnectController(2, Input::UNCONNECTED);
  input.ConnectController(3, Input::UNCONNECTED);
  input.ConnectController(4, Input::UNCONNECTED);
  for (auto& pad : impl_->controllers.pad) { pad.buttons = 0; pad.mic = 0; pad.allowSimulAxes = true; }

  r = machine.Power(true);
  if (NES_FAILED(r)) return nstErr(Err::RomInvalid, "power on", r);
  impl_->videoOut.pixels = video_.data();
  impl_->videoOut.pitch = kVideoWidth * 4;
  loaded_ = true;
  frame_ = 0;
  audioCount_ = 0;
  std::fill(video_.begin(), video_.end(), 0xFF000000u);
  std::fill(codes_.begin(), codes_.end(), uint16_t(0x0F));  // $0F = black, like the cleared picture
  codesBurstPhase_ = 0;
  codesFrame_ = 0;
  return Status::Ok();
}

Status NestopiaCore::powerCycle() {
  if (!loaded_) return Error(Err::InvalidArg, "no ROM loaded");
  Machine machine(impl_->emu);
  machine.SetRamPowerState(0);
  Nes::Result r = machine.Reset(true);  // hard reset == power cycle (RAM re-initialised)
  if (NES_FAILED(r)) return nstErr(Err::StateError, "power cycle", r);
  return Status::Ok();
}

Status NestopiaCore::softReset() {
  if (!loaded_) return Error(Err::InvalidArg, "no ROM loaded");
  Nes::Result r = Machine(impl_->emu).Reset(false);
  if (NES_FAILED(r)) return nstErr(Err::StateError, "soft reset", r);
  return Status::Ok();
}

Status NestopiaCore::stepFrame(uint8_t p1, uint8_t p2, bool renderVideo) {
  if (!loaded_) return Error(Err::InvalidArg, "no ROM loaded");
  Impl& im = *impl_;
  im.controllers.pad[0].buttons = p1;
  im.controllers.pad[1].buttons = p2;
  audioCount_ = audioSamplesForFrame(frame_);
  im.soundOut.samples[0] = audio_.data();
  im.soundOut.length[0] = static_cast<unsigned>(audioCount_);
  im.soundOut.samples[1] = nullptr;
  im.soundOut.length[1] = 0;
  Nes::Result r = im.emu.Execute(renderVideo ? &im.videoOut : nullptr, &im.soundOut, &im.controllers);
  if (NES_FAILED(r)) return nstErr(Err::StateError, "execute", r);
  if (renderVideo) {
    for (auto& px : video_) px |= 0xFF000000u;
    // Display-only copy of the PPU output the RGB picture was converted from (read, never
    // written: emulation and every hash are unaffected).
    Nes::Core::Machine& machine = static_cast<Nes::Core::Machine&>(im.emu);
    const Nes::Core::Video::Screen::Pixel* src = machine.ppu.GetScreen().pixels;
    for (size_t i = 0; i < codes_.size(); ++i) codes_[i] = uint16_t(src[i] & 0x1FF);
    codesBurstPhase_ = burstPhaseAtFrameEnd(machine.cpu.GetMonotonicCycles() - machine.cpu.GetCycles());
    codesFrame_ = frame_;
  }
  ++frame_;
  return Status::Ok();
}

Status NestopiaCore::saveState(std::vector<uint8_t>& out) {
  if (!loaded_) return Error(Err::InvalidArg, "no ROM loaded");
  std::ostringstream os(std::ios::out | std::ios::binary);
  Nes::Result r = Machine(impl_->emu).SaveState(os, Machine::NO_COMPRESSION);
  if (NES_FAILED(r)) return nstErr(Err::StateError, "save state", r);
  const std::string s = os.str();
  stateenv::write(out, compatId(), frame_, reinterpret_cast<const uint8_t*>(s.data()), s.size());
  return Status::Ok();
}

Status NestopiaCore::loadState(const uint8_t* data, size_t size) {
  if (!loaded_) return Error(Err::InvalidArg, "no ROM loaded");
  uint64_t frame;
  const uint8_t* p;
  size_t n;
  RN_TRY(stateenv::read(data, size, compatId(), frame, p, n));
  std::string bytes(reinterpret_cast<const char*>(p), n);
  std::istringstream in(bytes, std::ios::in | std::ios::binary);
  Nes::Result r = Machine(impl_->emu).LoadState(in);
  if (NES_FAILED(r)) return nstErr(Err::StateError, "load state", r);
  frame_ = frame;
  audioCount_ = 0;
  return Status::Ok();
}

const uint8_t* NestopiaCore::cpuRam() const {
  if (!impl_) return nullptr;
  // Read-only view of the CPU RAM through Nestopia's cheat API (no side effects).
  return Nes::Api::Cheats(impl_->emu).GetRam();
}

}  // namespace rn
