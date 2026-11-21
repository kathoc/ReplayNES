#include "core/FbneoCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <unistd.h>
#include <vector>

#include "core/ICore.h"
#include "libretro.h"
#include "util/Hash.h"

// FBNeo's own driver API (C linkage in src/burn/burn.h), used for the content identity and the
// pixel aspect; the libretro API covers everything else.
extern "C" {
struct BurnRomInfo {
  char* szName;
  unsigned int nLen;
  unsigned int nCrc;
  unsigned int nType;
};
struct BurnInputInfo {
  char* szName;
  unsigned char nType;
  void* pVal;
  char* szInfo;
};
char* BurnDrvGetTextA(unsigned int i);
int BurnDrvGetInputInfo(struct BurnInputInfo* pii, unsigned int i);
int BurnDrvGetRomInfo(struct BurnRomInfo* pri, unsigned int i);
int BurnDrvGetAspect(int* pnXAspect, int* pnYAspect);
int BurnDrvGetIndex(char* szName);
extern unsigned int nBurnDrvActive;
extern unsigned int nBurnDrvCount;
extern "C" int BurnDrvGetHardwareCode();
extern int nReplayArcadeScanVersion;  // patched FBNeo (state.h): the state layout FBNeo scans (kStateLayout)
}

namespace rac {
namespace {

// ReplayArcade plays arcade games only: FinalBurn Neo's home systems (its HARDWARE_PREFIX_* of Mega Drive,
// PC Engine, Master System, SG-1000, ColecoVision, Game Gear, MSX, ZX Spectrum, NES, FDS, Neo Geo Pocket,
// Channel F, SNES, Bally Astrocade, GBA) are refused (burn.h).
bool isHomeSystem(int hardware) {
  switch ((unsigned(hardware) >> 24) & 0x7f) {
    case 0x0c: case 0x12: case 0x17: case 0x18: case 0x19: case 0x1a: case 0x1c: case 0x1d:
    case 0x1e: case 0x1f: case 0x20: case 0x21: case 0x22: case 0x24: case 0x25: return true;
    default: return false;
  }
}

// The state layout this build writes (patched FBNeo, state.h; burn.cpp sets nReplayArcadeScanVersion to it).
constexpr int kStateLayout = 3;
constexpr unsigned kDrvName = 0;             // DRV_NAME
constexpr unsigned kRomNoDump = 1u << 28;    // BRF_NODUMP
constexpr unsigned char kAnalogRel = 4;      // BIT_ANALOG_REL: a dial / trackball, read as a speed
// FBNeo's libretro input reads a BIT_ANALOG_REL axis (signed: a stick, a dial) as (value >> 5) less
// a dead zone of 200 (at its default analog speed): values below 6400 of 32767 do nothing.
constexpr float kRelativeDeadzone = 6400.f / 32767.f;
// Sets whose dial-like inputs are speeds (tools/make_fbneo_relative.py): FBNeo marks sticks
// BIT_ANALOG_REL too (After Burner II), so the type alone does not tell.
struct RelativeInputs { const char* set; const char* names; };
constexpr RelativeInputs kRelativeInputs[] = {
#include "core/FbneoRelative.inc"
};
// The dead zone of an analog position axis end to end (FBNeo's libretro input plus the driver's own),
// measured (tools/make_fbneo_analog_deadzones.py): the Sega flight sticks and wheels answer from a third
// of the travel, not from kRelativeDeadzone. The frontend starts a controller stick past it.
struct AnalogDeadzone { const char* set; const char* axis; float deadzone; };
constexpr AnalogDeadzone kAnalogDeadzones[] = {
#include "core/FbneoAnalogDeadzones.inc"
};
constexpr const char* kCompat = "fbneo-libretro@2b0c1c067+replayarcade-1";
constexpr const char* kStateMagic = "fbneo";

class FbneoCore;

// The one real machine and the callbacks FBNeo calls.
struct Host {
  std::mutex m;
  FbneoSettings settings;
  bool initialised = false;
  std::string loadedPath;            // content currently in FBNeo ("" = none)
  std::string systemDir, saveDir;    // empty directories: no hiscore.dat, no NVRAM from earlier runs
  // The same as handed to FBNeo: its retro_load_game_common copies sizeof(g_save_dir) (MAX_PATH) bytes from the
  // pointer, past the end of a std::string's text (a read overrun, 2026-11-22, Guard Malloc): zero-padded
  // buffers larger than any MAX_PATH.
  char systemDirC[4096] = {}, saveDirC[4096] = {};
  // The picture format FBNeo set (RETRO_ENVIRONMENT_SET_PIXEL_FORMAT): XRGB8888, or RGB565 for a driver that
  // draws 16-bit colour only (BDF_16BIT_ONLY: Cave, ...). Until 2026-11-22 only XRGB8888 was accepted: such
  // a driver got no format, a 0-byte frame buffer (nBurnBpp 0) and wrote its frame over the heap (DonPachi
  // clears 320 x 240 x 2 bytes with 0xFF at its first frame; the app crashed in the library's pictures).
  retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_XRGB8888;
  std::string tempRoot;
  unsigned loads = 0;
  retro_system_av_info av{};
  unsigned rotation = 0;
  std::vector<uint8_t> powerOn;      // machine state right after loading, before any frame
  // Size of this content's states in each layout version (state.h): 1 = 0.2.1 and earlier, 2 = 0.3.0 and
  // 0.3.1, 3 = now. Index 0 unused.
  size_t layoutSize[4] = {0, 0, 0, 0};
  rn::InputLayout layout;            // from FBNeo's input descriptors for the loaded content
  struct AxisSource { unsigned port, index, id; size_t layoutByte; };
  std::vector<AxisSource> axes;
  std::string contentId;
  uint32_t samplesPerFrame = 0;
  FbneoCore* owner = nullptr;        // virtual machine whose state is in FBNeo now
  // Per-frame I/O of the frame being run.
  const rn::InputRecord* input = nullptr;
  FbneoCore* target = nullptr;
  bool collectDescriptors = false;
  std::vector<retro_input_descriptor> descriptors;
  std::vector<std::string> descriptorNames;
  // The temporary folders are this process's alone: removed at exit.
  ~Host() {
    std::error_code ec;
    if (!tempRoot.empty()) std::filesystem::remove_all(tempRoot, ec);
  }
};

Host& host() {
  static Host h;
  return h;
}

std::string optionValue(const char* key) {
  // Fixed by ReplayArcade (determinism and the engine's audio/video contract).
  static const std::map<std::string, std::string> fixed = {
      {"fbneo-hiscores", "disabled"},          // hiscore.dat injection is an outside input
      {"fbneo-diagnostic-input", "None"},      // "hold start" would enter the service mode
      {"fbneo-samplerate", "48000"},
      {"fbneo-allow-depth-32", "enabled"},     // XRGB8888: the engine's BGRA without conversion
      {"fbneo-force-60hz", "disabled"},
      {"fbneo-lowpass-filter", "disabled"},
      {"fbneo-frameskip-type", "No skipping"},
  };
  auto f = fixed.find(key);
  if (f != fixed.end()) return f->second;
  auto o = host().settings.options.find(key);
  return o != host().settings.options.end() ? o->second : std::string();
}

bool environment(unsigned cmd, void* data);
void videoRefresh(const void* data, unsigned w, unsigned h, size_t pitch);
size_t audioBatch(const int16_t* data, size_t frames);
void audioSample(int16_t l, int16_t r) {
  int16_t s[2] = {l, r};
  audioBatch(s, 1);
}
void inputPoll() {}
int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id);

// Retropad ids FBNeo maps arcade inputs to, and the action names ReplayArcade gives them.
const char* buttonName(unsigned id) {
  switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP: return "up";
    case RETRO_DEVICE_ID_JOYPAD_DOWN: return "down";
    case RETRO_DEVICE_ID_JOYPAD_LEFT: return "left";
    case RETRO_DEVICE_ID_JOYPAD_RIGHT: return "right";
    case RETRO_DEVICE_ID_JOYPAD_B: return "b1";
    case RETRO_DEVICE_ID_JOYPAD_A: return "b2";
    case RETRO_DEVICE_ID_JOYPAD_Y: return "b3";
    case RETRO_DEVICE_ID_JOYPAD_X: return "b4";
    case RETRO_DEVICE_ID_JOYPAD_L: return "b5";
    case RETRO_DEVICE_ID_JOYPAD_R: return "b6";
    case RETRO_DEVICE_ID_JOYPAD_L2: return "b7";
    case RETRO_DEVICE_ID_JOYPAD_R2: return "b8";
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return "coin";
    case RETRO_DEVICE_ID_JOYPAD_START: return "start";
    case RETRO_DEVICE_ID_JOYPAD_L3: return "l3";
    case RETRO_DEVICE_ID_JOYPAD_R3: return "service";
  }
  return nullptr;
}

// Record bytes of a port's 16 retropad bits: low byte, high byte (byte 2 is the events).
size_t lowByte(unsigned port) { return port; }            // ports 0 and 1: bytes 0 and 1
size_t highByte(unsigned port) { return 3 + port; }       // bytes 3 and 4
constexpr unsigned kMaxPorts = 2;
constexpr size_t kFirstAxisByte = 5;

// FBNeo's libretro port splits paths at the platform's own separator only (extract_directory looks
// for '\\' on Windows), while ReplayNES's paths may use '/' there: "C:\\ROM/gradius.zip" would make
// it look for the ROMs in "C:\\". Paths handed to FBNeo use the native separator.
std::string nativePath(std::string p) {
#ifdef _WIN32
  std::replace(p.begin(), p.end(), '/', '\\');
#endif
  return p;
}

// Builds the layout from FBNeo's descriptors: one button per retropad id a game uses (labelled
// with FBNeo's name for it), one axis per analog input. Fake-digital entries map to the d-pad.
void buildLayout(Host& h) {
  rn::InputLayout l;
  l.name = "fbneo:" + std::string(BurnDrvGetTextA(kDrvName));
  l.recordBytes = uint32_t(kFirstAxisByte);
  h.axes.clear();
  std::map<std::pair<unsigned, unsigned>, std::string> buttons;
  // Dials of this set by (port, name): FBNeo names them "P<n> <name>" (or "<name>" for a one-player
  // machine); the descriptors drop "P<n> ".
  std::set<std::string> dials;
  const std::string set = BurnDrvGetTextA(kDrvName);
  for (const RelativeInputs& r : kRelativeInputs)
    if (set == r.set)
      for (size_t a = 0, b; a <= std::strlen(r.names); a = b + 1) {
        b = std::string(r.names).find('|', a);
        if (b == std::string::npos) b = std::strlen(r.names);
        dials.insert(std::string(r.names, a, b - a));
      }
  // Every BIT_ANALOG_REL input has the dead zone; the dials among them are speeds.
  std::set<std::pair<unsigned, std::string>> signedInputs, relative;
  BurnInputInfo bii{};
  for (unsigned i = 0; BurnDrvGetInputInfo(&bii, i) == 0; ++i) {
    if (bii.nType != kAnalogRel || !bii.szName) continue;
    std::string n = bii.szName;
    const bool player = n.size() > 3 && n[0] == 'P' && n[1] >= '1' && n[1] <= '9' && n[2] == ' ';
    std::pair<unsigned, std::string> key{player ? unsigned(n[1] - '1') : 0u, player ? n.substr(3) : n};  // "Throttle": player 1
    signedInputs.insert(key);
    if (dials.count(key.second)) relative.insert(key);
  }
  for (size_t i = 0; i < h.descriptors.size(); ++i) {
    const retro_input_descriptor& d = h.descriptors[i];
    if (d.port >= kMaxPorts) continue;
    if (d.device == RETRO_DEVICE_JOYPAD && buttonName(d.id)) {
      auto key = std::make_pair(d.port, d.id);
      if (!buttons.count(key)) buttons[key] = h.descriptorNames[i];
    } else if (d.device == RETRO_DEVICE_ANALOG && d.index <= 1 && d.id <= 1) {
      bool seen = false;
      for (auto& a : h.axes) seen = seen || (a.port == d.port && a.index == d.index && a.id == d.id);
      if (seen) continue;
      std::string axisName = std::string(d.index == 0 ? "" : "r") + (d.id == 0 ? "x" : "y");
      size_t byte = l.recordBytes;
      l.recordBytes += 2;
      rn::InputLayout::Axis axis{"p" + std::to_string(d.port + 1) + ".axis." + axisName, h.descriptorNames[i], uint16_t(byte)};
      axis.relative = relative.count({d.port, h.descriptorNames[i]}) != 0;
      if (signedInputs.count({d.port, h.descriptorNames[i]})) axis.deadzone = kRelativeDeadzone;
      const std::string p1Axis = "p1.axis." + axisName;  // the table has player 1's; player 2's are the same
      for (const AnalogDeadzone& m : kAnalogDeadzones)
        if (set == m.set && p1Axis == m.axis) axis.deadzone = m.deadzone;
      l.axes.push_back(axis);
      h.axes.push_back({d.port, d.index, d.id, byte});
    }
  }
  // A stick whose two axes are both positions (not a dial, not FBNeo's fake stick for the d-pad): a
  // flight stick or a gun's aim, whose travel is a square ("square" hint: a round controller stick's
  // diagonals reach both ends).
  auto real = [](const rn::InputLayout::Axis& a) { return !a.relative && a.label.find("Fake") == std::string::npos; };
  for (auto& a : l.axes) {
    const size_t k = a.id.rfind('.');
    const std::string name = a.id.substr(k + 1);  // x, y, rx, ry
    std::string other = a.id.substr(0, k + 1) + (name.back() == 'x' ? name.substr(0, name.size() - 1) + "y" : name.substr(0, name.size() - 1) + "x");
    for (auto& b : l.axes)
      if (b.id == other && real(a) && real(b)) a.square = true;
  }
  for (auto& kv : buttons) {
    unsigned port = kv.first.first, id = kv.first.second;
    std::string label = kv.second;
    if (label.find("Fake Digital") != std::string::npos || label.find("Fake Left Stick") != std::string::npos)
      label.clear();  // the d-pad: the frontend names it
    rn::InputLayout::Button b;
    b.id = "p" + std::to_string(port + 1) + "." + buttonName(id);
    b.label = label;
    b.byte = uint16_t(id < 8 ? lowByte(port) : highByte(port));
    b.bit = uint8_t(id % 8);
    l.buttons.push_back(b);
  }
  // Menus list buttons in layout order: per player, the numbered buttons, then coin and start,
  // then the d-pad (retropad id order would put Button 2 after the d-pad). The record bits stay.
  auto rank = [](const rn::InputLayout::Button& b) {
    static const char* order[] = {"b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8", "coin", "start",
                                  "service", "l3", "up", "down", "left", "right"};
    std::string name = b.id.substr(b.id.find('.') + 1);
    int i = 0;
    while (i < 16 && name != order[i]) ++i;
    return std::make_pair(b.id.substr(0, b.id.find('.')), i);
  };
  std::stable_sort(l.buttons.begin(), l.buttons.end(),
                   [&](const rn::InputLayout::Button& a, const rn::InputLayout::Button& b) { return rank(a) < rank(b); });
  for (unsigned port = 0; port < kMaxPorts; ++port) {
    std::string pre = "p" + std::to_string(port + 1) + ".";
    auto has = [&](const std::string& id) {
      for (auto& b : l.buttons)
        if (b.id == id) return true;
      return false;
    };
    if (has(pre + "up") && has(pre + "down")) l.opposite.push_back({pre + "up", pre + "down"});
    if (has(pre + "left") && has(pre + "right")) l.opposite.push_back({pre + "left", pre + "right"});
  }
  h.layout = l;
}

// Set name + the CRC of every ROM the driver needs (parent and BIOS included): the same for a set
// repacked with other compression or merged / split differently.
std::string contentIdOfLoaded() {
  std::vector<uint32_t> crcs;
  BurnRomInfo ri{};
  for (unsigned i = 0; BurnDrvGetRomInfo(&ri, i) == 0; ++i) {
    if (ri.nCrc && !(ri.nType & kRomNoDump)) crcs.push_back(ri.nCrc);
    ri = BurnRomInfo{};
  }
  std::sort(crcs.begin(), crcs.end());
  rn::Hasher64 h;
  h.update(crcs.data(), crcs.size() * sizeof(uint32_t));
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h.digest());
  return std::string("fbneo:") + BurnDrvGetTextA(kDrvName) + ":" + hex;
}

class FbneoCore final : public rn::ICore {
 public:
  ~FbneoCore() override {
    std::lock_guard<std::mutex> g(host().m);
    if (host().owner == this) host().owner = nullptr;
  }

  rn::Status loadROM(const uint8_t*, size_t) override {
    return rn::Error(rn::Err::UnsupportedFormat, "FinalBurn Neo loads arcade sets by path");
  }

  rn::Status loadContent(const std::string& path, const uint8_t*, size_t) override {
    std::lock_guard<std::mutex> g(host().m);
    RN_TRY(ensureLoaded(path));
    path_ = path;
    loaded_ = true;
    frame_ = 0;
    count_ = 0;
    parked_ = host().powerOn;
    if (host().owner == this) host().owner = nullptr;  // take it again from the power-on state
    return take();
  }

  rn::Status powerCycle() override {
    std::lock_guard<std::mutex> g(host().m);
    if (!loaded_) return rn::Error(rn::Err::InvalidArg, "no content loaded");
    RN_TRY(take());
    if (!retro_unserialize(host().powerOn.data(), host().powerOn.size()))
      return rn::Error(rn::Err::StateError, "power-on state did not load");
    return rn::Status::Ok();
  }

  rn::Status softReset() override {
    std::lock_guard<std::mutex> g(host().m);
    if (!loaded_) return rn::Error(rn::Err::InvalidArg, "no content loaded");
    RN_TRY(take());
    retro_reset();
    return rn::Status::Ok();
  }

  rn::Status stepFrame(uint8_t p1, uint8_t p2, bool render) override {
    return stepInput(rn::InputRecord{p1, p2, 0}, render);
  }

  rn::Status stepInput(const rn::InputRecord& r, bool) override {
    std::lock_guard<std::mutex> g(host().m);
    if (!loaded_) return rn::Error(rn::Err::InvalidArg, "no content loaded");
    RN_TRY(take());
    Host& h = host();
    h.input = &r;
    h.target = this;
    count_ = 0;
    retro_run();
    h.input = nullptr;
    h.target = nullptr;
    if (count_ != h.samplesPerFrame)
      return rn::Error(rn::Err::Internal, "FinalBurn Neo produced " + std::to_string(count_) + " sample frames, expected " +
                                              std::to_string(h.samplesPerFrame));
    ++frame_;
    return rn::Status::Ok();
  }

  uint64_t frameIndex() const override { return frame_; }
  const uint32_t* video() const override { return video_.data(); }
  const int16_t* audio(size_t* count) const override {
    *count = count_;
    return audio_.data();
  }

  rn::MediaInfo mediaInfo() const override { return media_; }
  rn::VideoFrame videoFrame() const override { return rn::VideoFrame{video_.data(), width_, height_, width_}; }
  uint64_t samplesBefore(uint64_t frame) const override { return frame * spf_; }
  rn::InputLayout inputLayout() const override { return layout_; }
  std::string contentId() const override { return contentId_; }

  rn::Status saveState(std::vector<uint8_t>& out) override {
    std::lock_guard<std::mutex> g(host().m);
    if (!loaded_) return rn::Error(rn::Err::InvalidArg, "no content loaded");
    if (host().owner != this) {
      rn::stateenv::write(out, kCompat, frame_, parked_.data(), parked_.size());
      return rn::Status::Ok();
    }
    // Serialized in place, after the envelope's header: a state is megabytes (Sega System 32: 22 MB),
    // and a buffer copied into the envelope cost a copy of it on every dense checkpoint.
    const size_t size = host().powerOn.size();
    uint8_t* payload = rn::stateenv::begin(out, kCompat, frame_, size);
    if (!retro_serialize(payload, size)) return rn::Error(rn::Err::StateError, "FinalBurn Neo could not save");
    return rn::Status::Ok();
  }

  rn::Status loadState(const uint8_t* data, size_t size) override {
    std::lock_guard<std::mutex> g(host().m);
    if (!loaded_) return rn::Error(rn::Err::InvalidArg, "no content loaded");
    uint64_t frame;
    const uint8_t* payload;
    size_t n;
    RN_TRY(rn::stateenv::read(data, size, kCompat, frame, payload, n));
    if (layoutOfSize(n) == 0)
      return rn::Error(rn::Err::StateError, "state belongs to other content");
    if (host().owner == this) {
      if (!unserialize(payload, n)) return rn::Error(rn::Err::StateError, "FinalBurn Neo refused the state");
    } else {
      parked_.assign(payload, payload + n);
    }
    frame_ = frame;
    count_ = 0;
    return rn::Status::Ok();
  }

  std::string compatId() const override { return kCompat; }
  std::string buildId() const override { return std::string(kCompat) + " (" + kStateMagic + ")"; }
  uint32_t stateFormatVersion() const override { return 1; }
  rn::CoreKind kind() const override { return rn::CoreKind::External; }

  // Called by the callbacks while this instance runs a frame (host mutex held).
  void receiveVideo(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;  // a duplicate frame: keep the last picture
    width_ = std::min<unsigned>(w, media_.maxWidth);
    height_ = std::min<unsigned>(h, media_.maxHeight);
    if (host().pixelFormat == RETRO_PIXEL_FORMAT_RGB565) {
      // 16-bit drivers: RGB565 widened to the engine's 0xAARRGGBB (5 / 6 bits repeated into the low bits).
      for (unsigned y = 0; y < height_; ++y) {
        const uint16_t* src = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(data) + y * pitch);
        uint32_t* dst = video_.data() + size_t(y) * width_;
        for (unsigned x = 0; x < width_; ++x) {
          const uint32_t p = src[x], r = p >> 11 & 31, g = p >> 5 & 63, b = p & 31;
          dst[x] = 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
        }
      }
      return;
    }
    for (unsigned y = 0; y < height_; ++y) {
      const uint32_t* src = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(data) + y * pitch);
      uint32_t* dst = video_.data() + size_t(y) * width_;
      for (unsigned x = 0; x < width_; ++x) dst[x] = src[x] | 0xFF000000u;
    }
  }
  size_t receiveAudio(const int16_t* data, size_t frames) {
    size_t room = audio_.size() / 2 - count_;
    size_t n = std::min(frames, room);
    std::memcpy(audio_.data() + count_ * 2, data, n * 2 * sizeof(int16_t));
    count_ += n;
    return frames;
  }

 private:
  // Loads `path` into FBNeo if another content is there (host mutex held).
  // Unloads the content; its save directory (the NVRAM FBNeo writes at unload) goes with it, so
  // nothing a game wrote can reach a later power-on.
  static void unloadContent(Host& h) {
    retro_unload_game();
    std::error_code ec;
    std::filesystem::remove_all(h.saveDir, ec);
  }

  rn::Status ensureLoaded(const std::string& path) {
    Host& h = host();
    if (h.loadedPath == path) {
      copyContentInfo();
      return rn::Status::Ok();
    }
    if (h.owner) h.owner->park();
    h.owner = nullptr;
    if (!h.loadedPath.empty()) unloadContent(h);
    h.loadedPath.clear();
    // A fresh, empty save directory per load: NVRAM written at unload must not reach the next
    // power-on (projects replay from power-on). The temporary root is unique to this process, and
    // an existing folder is emptied anyway.
    h.saveDir = nativePath(h.tempRoot + "/save-" + std::to_string(++h.loads));
    {
      std::error_code ec;
      std::filesystem::remove_all(h.saveDir, ec);
      std::filesystem::create_directories(h.saveDir, ec);
    }
    h.rotation = 0;  // set again by the driver while loading (RETRO_ENVIRONMENT_SET_ROTATION)
    // A home system's set is refused before FBNeo starts on it (its drivers can crash on a broken file:
    // MegadriveExit after a failed init). The check after loading catches a set FBNeo picks by the folder.
    {
      const std::string set = std::filesystem::path(path).stem().string();
      const int index = BurnDrvGetIndex(const_cast<char*>(set.c_str()));
      if (index >= 0) {
        const unsigned saved = nBurnDrvActive;
        nBurnDrvActive = unsigned(index);
        const bool home = isHomeSystem(BurnDrvGetHardwareCode());
        nBurnDrvActive = saved;
        if (home)
          return rn::Error(rn::Err::UnsupportedFormat, "'" + path + "' is not an arcade game (ReplayArcade plays arcade games only)");
      }
    }
    retro_game_info gi{};
    const std::string native = nativePath(path);
    gi.path = native.c_str();
    if (!retro_load_game(&gi)) return rn::Error(rn::Err::RomInvalid, "FinalBurn Neo could not load '" + path + "'");
    // FBNeo's libretro frontend can answer true with no driver selected (an empty or broken zip): nothing
    // of the driver may be read then.
    if (nBurnDrvActive >= nBurnDrvCount) {
      unloadContent(h);
      return rn::Error(rn::Err::RomInvalid, "FinalBurn Neo could not load '" + path + "'");
    }
    if (isHomeSystem(BurnDrvGetHardwareCode())) {
      unloadContent(h);
      return rn::Error(rn::Err::UnsupportedFormat, "'" + path + "' is not an arcade game (ReplayArcade plays arcade games only)");
    }
    retro_get_system_av_info(&h.av);
    // Power-on state first; then one frame to receive the input descriptors (FBNeo sends them
    // on its first frame); then back to power-on.
    size_t size = retro_serialize_size();
    for (int v = 1; v <= kStateLayout; ++v) {
      nReplayArcadeScanVersion = v;
      h.layoutSize[v] = retro_serialize_size();
    }
    nReplayArcadeScanVersion = kStateLayout;
    h.powerOn.assign(size, 0);
    if (!size || !retro_serialize(h.powerOn.data(), size)) {
      unloadContent(h);
      return rn::Error(rn::Err::StateError, "FinalBurn Neo cannot save states for '" + path + "'");
    }
    h.descriptors.clear();
    h.descriptorNames.clear();
    h.collectDescriptors = true;
    rn::InputRecord none;
    h.input = &none;
    h.target = nullptr;
    retro_run();
    h.input = nullptr;
    h.collectDescriptors = false;
    if (!retro_unserialize(h.powerOn.data(), size)) {
      unloadContent(h);
      return rn::Error(rn::Err::StateError, "FinalBurn Neo cannot restore its power-on state");
    }
    buildLayout(h);
    rn::Status lv = h.layout.validate();
    if (!lv.ok()) {
      unloadContent(h);
      return rn::Error(rn::Err::Internal, "input layout from FinalBurn Neo is invalid: " + lv.message);
    }
    h.contentId = contentIdOfLoaded();
    h.samplesPerFrame = uint32_t(std::lround(h.av.timing.sample_rate / h.av.timing.fps));
    h.loadedPath = path;
    copyContentInfo();
    return rn::Status::Ok();
  }

  void copyContentInfo() {
    Host& h = host();
    media_ = rn::MediaInfo{};
    media_.maxWidth = std::max(h.av.geometry.max_width, h.av.geometry.base_width);
    media_.maxHeight = std::max(h.av.geometry.max_height, h.av.geometry.base_height);
    // FBNeo's frame rate is in hundredths of a hertz (59.63 Hz = 5963 / 100).
    media_.fpsNum = uint64_t(std::llround(h.av.timing.fps * 100.0));
    media_.fpsDen = 100;
    media_.audioChannels = 2;
    int ax = 4, ay = 3;
    BurnDrvGetAspect(&ax, &ay);
    // ax:ay is the shown (rotated) aspect; the pixel aspect describes an unrotated source pixel:
    // shown aspect = w * p / hh unrotated, its inverse when turned a quarter.
    unsigned w = h.av.geometry.base_width, hh = h.av.geometry.base_height;
    bool vertical = h.rotation % 2 == 1;
    uint64_t num = uint64_t(vertical ? ay : ax) * hh;
    uint64_t den = uint64_t(vertical ? ax : ay) * w;
    uint64_t gcd = std::gcd(num, den);
    media_.pixelAspectNum = uint32_t(num / (gcd ? gcd : 1));
    media_.pixelAspectDen = uint32_t(den / (gcd ? gcd : 1));
    // libretro reports counter-clockwise quarter turns; MediaInfo counts clockwise ones.
    media_.rotation = (4 - h.rotation) % 4;
    spf_ = h.samplesPerFrame;
    layout_ = h.layout;
    contentId_ = h.contentId;
    video_.assign(size_t(media_.maxWidth) * media_.maxHeight, 0xFF000000u);
    width_ = w;
    height_ = hh;
    audio_.assign(size_t(spf_ + 64) * 2, 0);
  }

  // The layout version of a state of n bytes, the newest that has that size (each version only adds
  // bytes, so equal sizes have the same layout); 0 = none of this content's.
  static int layoutOfSize(size_t n) {
    for (int v = kStateLayout; v >= 1; --v)
      if (host().layoutSize[v] == n) return v;
    return 0;
  }

  // A state of this layout, or one saved before it (a project from an earlier version; its size tells).
  // Loading one leaves out what its version did not save.
  static bool unserialize(const uint8_t* data, size_t n) {
    const int v = layoutOfSize(n);
    if (v == 0) return false;
    nReplayArcadeScanVersion = v;
    const bool ok = retro_unserialize(data, n);
    nReplayArcadeScanVersion = kStateLayout;
    return ok;
  }

  rn::Status serializeInto(std::vector<uint8_t>& out) {
    out.assign(host().powerOn.size(), 0);
    if (!retro_serialize(out.data(), out.size())) return rn::Error(rn::Err::StateError, "FinalBurn Neo could not save");
    return rn::Status::Ok();
  }

  // Saves this instance's machine (it is giving FBNeo to another instance).
  void park() {
    if (!serializeInto(parked_).ok()) parked_ = host().powerOn;  // never seen in practice; keeps a valid state
  }

  // Makes FBNeo hold this instance's machine (host mutex held).
  rn::Status take() {
    Host& h = host();
    if (h.owner == this) return rn::Status::Ok();
    if (h.loadedPath != path_) RN_TRY(ensureLoaded(path_));
    if (h.owner) h.owner->park();
    h.owner = nullptr;
    if (!unserialize(parked_.data(), parked_.size()))
      return rn::Error(rn::Err::StateError, "FinalBurn Neo refused this core's state");
    h.owner = this;
    return rn::Status::Ok();
  }

  std::string path_;
  bool loaded_ = false;
  uint64_t frame_ = 0;
  std::vector<uint8_t> parked_;
  rn::MediaInfo media_;
  rn::InputLayout layout_;
  std::string contentId_;
  uint32_t spf_ = 0;
  std::vector<uint32_t> video_;
  unsigned width_ = 0, height_ = 0;
  std::vector<int16_t> audio_;
  size_t count_ = 0;
};

bool environment(unsigned cmd, void* data) {
  Host& h = host();
  switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      std::snprintf(h.systemDirC, sizeof(h.systemDirC), "%s", h.systemDir.c_str());
      *(const char**)data = h.systemDirC;
      return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      std::snprintf(h.saveDirC, sizeof(h.saveDirC), "%s", h.saveDir.c_str());
      *(const char**)data = h.saveDirC;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
      const retro_pixel_format f = *(const retro_pixel_format*)data;
      if (f != RETRO_PIXEL_FORMAT_XRGB8888 && f != RETRO_PIXEL_FORMAT_RGB565) return false;
      h.pixelFormat = f;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
      if (data) *(int*)data = RETRO_SAVESTATE_CONTEXT_NORMAL;
      return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
      if (data) *(int*)data = 3;
      return true;
    case RETRO_ENVIRONMENT_SET_ROTATION: h.rotation = *(const unsigned*)data % 4; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* v = (retro_variable*)data;
      if (!v || !v->key) return false;
      static thread_local std::string value;
      value = optionValue(v->key);
      if (value.empty()) return false;
      v->value = value.c_str();
      return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      if (h.collectDescriptors && data) {
        for (auto* d = (const retro_input_descriptor*)data; d->description; ++d) {
          h.descriptors.push_back(*d);
          h.descriptorNames.push_back(d->description);
        }
      }
      return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
      return true;
  }
  return false;
}

void videoRefresh(const void* data, unsigned w, unsigned h, size_t pitch) {
  if (host().target) host().target->receiveVideo(data, w, h, pitch);
}

size_t audioBatch(const int16_t* data, size_t frames) {
  return host().target ? host().target->receiveAudio(data, frames) : frames;
}

int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
  const rn::InputRecord* r = host().input;
  if (!r || port >= kMaxPorts) return 0;
  if (device == RETRO_DEVICE_JOYPAD) {
    unsigned bits = r->byte(lowByte(port)) | unsigned(r->byte(highByte(port))) << 8;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return int16_t(bits);
    return id < 16 ? int16_t((bits >> id) & 1) : 0;
  }
  if (device == RETRO_DEVICE_ANALOG) {
    for (auto& a : host().axes)
      if (a.port == port && a.index == index && a.id == id)
        return int16_t(r->byte(a.layoutByte) | r->byte(a.layoutByte + 1) << 8);
  }
  return 0;
}

}  // namespace

bool isArcadeFile(const std::string& path) {
  std::string name = std::filesystem::path(path).stem().string();
  std::lock_guard<std::mutex> g(host().m);  // no frame runs while the driver selection is borrowed
  int index = BurnDrvGetIndex(const_cast<char*>(name.c_str()));
  if (index < 0) return true;
  const unsigned saved = nBurnDrvActive;
  nBurnDrvActive = unsigned(index);
  const bool home = isHomeSystem(BurnDrvGetHardwareCode());
  nBurnDrvActive = saved;
  return !home;
}

std::string contentIdForPath(const std::string& path) {
  std::string name = std::filesystem::path(path).stem().string();
  std::lock_guard<std::mutex> g(host().m);  // no frame runs while the driver selection is borrowed
  int index = BurnDrvGetIndex(const_cast<char*>(name.c_str()));
  if (index < 0) return {};
  const unsigned saved = nBurnDrvActive;
  nBurnDrvActive = unsigned(index);
  std::string id = isHomeSystem(BurnDrvGetHardwareCode()) ? std::string() : contentIdOfLoaded();
  nBurnDrvActive = saved;
  return id;
}

void registerFbneoCore(const FbneoSettings& settings) {
  {
    std::lock_guard<std::mutex> g(host().m);
    Host& h = host();
    h.settings = settings;
    if (!h.initialised) {
      std::error_code ec;
      std::string base = std::filesystem::temp_directory_path(ec).string();
      // Unique per process: a folder name made of the process id alone was reused by later runs
      // (Wine's small, recycled ids), and an earlier run's NVRAM files in it reached this run's games.
      std::random_device rd;
      for (int attempt = 0; attempt < 100 && h.tempRoot.empty(); ++attempt) {
        char tag[32];
        std::snprintf(tag, sizeof tag, "%08x%08x", unsigned(rd()), unsigned(rd()));
        std::string candidate = base + "/replayarcade-fbneo-" + std::to_string(::getpid()) + "-" + tag;
        if (std::filesystem::create_directory(candidate, ec)) h.tempRoot = candidate;
      }
      if (h.tempRoot.empty()) std::fprintf(stderr, "FinalBurn Neo: cannot create a temporary folder in %s\n", base.c_str());
      h.systemDir = nativePath(h.tempRoot + "/system");
      std::filesystem::create_directories(h.systemDir, ec);
      retro_set_environment(environment);
      retro_set_video_refresh(videoRefresh);
      retro_set_audio_sample(audioSample);
      retro_set_audio_sample_batch(audioBatch);
      retro_set_input_poll(inputPoll);
      retro_set_input_state(inputState);
      retro_init();
      h.initialised = true;
    }
  }
  // All instances take turns on FBNeo's one machine (CapSharesMachine): the app does not run them in parallel.
  rn::registerExternalCore({"fbneo", kCompat,
                            uint32_t(rn::CapAudio | rn::CapState | rn::CapSoftReset | rn::CapRecording | rn::CapSharesMachine),
                            [] { return std::unique_ptr<rn::ICore>(new FbneoCore()); }});
}

}  // namespace rac
