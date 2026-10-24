// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>

#include "audio_format.h"
#include "audio_sink.h"
#include "cadence.h"
#include "display_scheduler.h"
#include "support/rn_test.h"

using namespace rnl;

TEST_CASE("desktop media audio: NES queue and stream defaults stay exact") {
  rn_media_info media{256, 240, RN_FPS_NUM, RN_FPS_DEN, 1, 8, 7, 0};
  auto f = AudioStreamFormat::forMedia(media, 48000);
  CHECK_EQ(f.streamRate(), 48000);
  CHECK_EQ(f.channels, 1u);
  CHECK_EQ(f.bytesPerFrame(), size_t(2));
  CHECK_EQ(f.samplesPerPush, 800.0);
  CHECK_EQ(f.targetFill(512), 1568.0);
  CHECK_EQ(f.targetFill(1024), 2336.0);
}

TEST_CASE("desktop media audio: stereo stream rate and queue units follow the core") {
  rn_media_info media{320, 224, 5963, 100, 2, 1, 1, 1};
  auto f = AudioStreamFormat::forMedia(media, 805.0 * 59.63);
  CHECK(std::fabs(f.sampleRate - 48002.15) < 1e-8);
  CHECK_EQ(f.streamRate(), 48002);
  CHECK_EQ(f.channels, 2u);
  CHECK_EQ(f.bytesPerFrame(), size_t(4));
  CHECK(std::fabs(f.samplesPerPush - 805.0) < 1e-10);
  CHECK(std::fabs(f.targetFill(512) - (768 * 48002.15 / 48000 + 805)) < 1e-8);
  auto low = AudioStreamFormat::forMedia(media, 32000);
  CHECK_EQ(low.streamRate(), 32000);
  CHECK(std::fabs(low.targetFill(512) - (512 + 32000 / 59.63)) < 1e-8);
}

TEST_CASE("desktop media cadence: configurable game period and tolerance") {
  const double period = 100.0 / 5963;
  CHECK(Cadence::classify(1.0 / 60, period).kind == Cadence::Kind::free);
  auto wide = Cadence::classify(1.0 / 60, period, 0.015);
  CHECK(wide.kind == Cadence::Kind::locked);
  CHECK_EQ(wide.k, 1);
  CHECK_EQ(wide.emulationRate(1.0 / 60), 60.0);
  CHECK(std::fabs(Cadence::classify(1.0 / 100, period).emulationRate(1.0 / 100) - 59.63) < 1e-10);
  DisplayScheduler scheduler;
  scheduler.seedRefresh(1.0 / 60);
  scheduler.setFramePeriod(period, 0.015);
  CHECK(scheduler.cadence().kind == Cadence::Kind::locked);
  CHECK_EQ(scheduler.framePeriod(), period);
  CHECK(std::fabs(scheduler.nextTarget(10, 0.003) - (10.003 + period)) < 1e-12);
}

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "core/ICore.h"
#include "emulation.h"

namespace {
class DesktopMediaCore final : public rn::ICore {
 public:
  rn::Status loadROM(const uint8_t*, size_t) override { return powerCycle(); }
  rn::Status powerCycle() override { frame_ = 0; return rn::Status::Ok(); }
  rn::Status softReset() override { return rn::Status::Ok(); }
  rn::Status stepFrame(uint8_t, uint8_t, bool render) override {
    ++frame_;
    if (render) {
      std::fill(pixels_.begin(), pixels_.end(), 0xDEADBEEFu);
      auto f = videoFrame();
      for (uint32_t y = 0; y < f.height; ++y)
        for (uint32_t x = 0; x < f.width; ++x)
          pixels_[size_t(y) * f.stride + x] = 0xFF000000u | (y << 8) | x;
    }
    for (size_t i = 0; i < audio_.size(); ++i) audio_[i] = i % 2 ? -1234 : 2345;
    return rn::Status::Ok();
  }
  uint64_t frameIndex() const override { return frame_; }
  const uint32_t* video() const override { return pixels_.data(); }
  const int16_t* audio(size_t* n) const override { *n = frame_ ? 805 : 0; return audio_.data(); }
  rn::MediaInfo mediaInfo() const override { return {384, 256, 5963, 100, 2, 1, 1, 1}; }
  rn::VideoFrame videoFrame() const override { return {pixels_.data(), frame_ % 2 ? 320u : 288u, 224, 384}; }
  uint64_t samplesBefore(uint64_t frame) const override { return frame * 805; }
  rn::Status saveState(std::vector<uint8_t>&) override { return rn::Error(rn::Err::UnsupportedFormat, "live only"); }
  rn::Status loadState(const uint8_t*, size_t) override { return rn::Error(rn::Err::UnsupportedFormat, "live only"); }
  std::string compatId() const override { return "desktop-media-audio/1"; }
  std::string buildId() const override { return "test"; }
  uint32_t stateFormatVersion() const override { return 1; }
  rn::CoreKind kind() const override { return rn::CoreKind::External; }
 private:
  uint64_t frame_ = 0;
  std::vector<uint32_t> pixels_ = std::vector<uint32_t>(384 * 256, 0xFF000000u);
  std::vector<int16_t> audio_ = std::vector<int16_t>(805 * 2);
};

struct StereoSink final : AudioSink {
  AudioStreamFormat format;
  size_t frames = 0;
  uint32_t channels = 0;
  bool muted = true;
  std::vector<int16_t> pcm;
  void setMuted(bool m) override { muted = m; }
  void configureMedia(const rn_media_info& m, double rate) override { format = AudioStreamFormat::forMedia(m, rate); }
  void push(const int16_t*, size_t) override { FAIL("channel-aware input required"); }
  void push(const int16_t* p, size_t n, uint32_t c) override {
    CHECK_FALSE(muted);
    frames = n;
    channels = c;
    pcm.assign(p, p + n * c);
  }
};
}

TEST_CASE("desktop media emulation: padded changing pictures and stereo audio reach their sinks") {
  rn::registerExternalCore({"desktop-media-audio", "desktop-media-audio/1", rn::CapAudio,
                           [] { return std::make_unique<DesktopMediaCore>(); }});
  struct Registration { ~Registration() { rn::unregisterExternalCore(); } } registration;
  auto path = std::filesystem::path(RNL_TEST_TMP) / "desktop-media-audio.bin";
  { std::ofstream out(path, std::ios::binary); out << "external test content"; }
  rn_session_options options;
  rn_session_options_init(&options);
  options.core = RN_CORE_EXTERNAL;
  rn_session* session = nullptr;
  REQUIRE_EQ(rn_session_new(path.string().c_str(), nullptr, &options, &session), RN_OK);
  std::unique_ptr<rn_input, decltype(&rn_input_free)> input(rn_input_new(), rn_input_free);
  StereoSink audio;
  EmulationController emu(input.get(), &audio);
  emu.install(session);
  CHECK_FALSE(emu.crtAvailable());
  CHECK_EQ(emu.media().max_width, 384u);
  CHECK_EQ(emu.media().rotation, 1u);
  CHECK(std::fabs(emu.framePeriod() - 100.0 / 5963) < 1e-12);
  CHECK(std::fabs(audio.format.sampleRate - 48002.15) < 1e-8);
  CHECK_EQ(audio.format.channels, 2u);
  emu.setCrtDisplay(true);
  emu.setFlashLevel(RN_FLASH_OFF);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(emu.tick().newPicture);
    auto f = emu.videoFrame();
    CHECK_EQ(f.width, i % 2 ? 288u : 320u);
    CHECK_EQ(f.height, 224u);
    CHECK_EQ(f.stride, f.width);
    CHECK_EQ(f.pixels[size_t(f.height - 1) * f.stride + f.width - 1],
             0xFF000000u | ((f.height - 1) << 8) | (f.width - 1));
    CHECK_EQ(audio.frames, size_t(805));
    CHECK_EQ(audio.channels, 2u);
    CHECK_EQ(audio.pcm.size(), size_t(1610));
    CHECK_EQ(audio.pcm[0], 2345);
    CHECK_EQ(audio.pcm[1], -1234);
    CHECK_EQ(audio.pcm.back(), -1234);
  }
  emu.setFlashLevel(RN_FLASH_STANDARD);
  REQUIRE(emu.tick().newPicture);
  CHECK_EQ(emu.videoFrame().width, 320u);
  emu.closeSession();
  CHECK_FALSE(emu.crtAvailable());
  std::filesystem::remove(path);
}
