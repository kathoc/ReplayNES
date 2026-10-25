// SPDX-License-Identifier: GPL-2.0-or-later
#include "support/rn_test.h"
#include "core/ICore.h"
#include "emulation.h"
#include "thumbnails.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace rnl {
struct ThumbnailSchedulingProbe {
  static size_t admitted(const ThumbnailManager& manager) { return manager.job_ ? manager.job_->next : 0; }
  static size_t workers(const ThumbnailManager& manager) { return manager.workers_.size(); }
};
}
namespace {
std::atomic<int> creations{0};
class ThumbnailCore final : public rn::ICore {
 public:
  ThumbnailCore() {
    ++creations;
    for (unsigned y = 0; y < 240; ++y) for (unsigned x = 0; x < 256; ++x)
      pixels_[y * 256 + x] = 0xff000000u | (x << 16) | (y << 8) | ((x ^ y) & 255);
  }
  rn::Status loadROM(const uint8_t*, size_t) override { return powerCycle(); }
  rn::Status powerCycle() override { frame_ = 0; return rn::Status::Ok(); }
  rn::Status softReset() override { return rn::Status::Ok(); }
  rn::Status stepFrame(uint8_t, uint8_t, bool) override { ++frame_; return rn::Status::Ok(); }
  uint64_t frameIndex() const override { return frame_; }
  const uint32_t* video() const override { return pixels_.data(); }
  const int16_t* audio(size_t* n) const override { *n = 0; return nullptr; }
  rn::Status saveState(std::vector<uint8_t>& out) override {
    out.resize(sizeof frame_); std::memcpy(out.data(), &frame_, sizeof frame_); return rn::Status::Ok();
  }
  rn::Status loadState(const uint8_t* bytes, size_t n) override {
    if (n != sizeof frame_) return rn::Error(rn::Err::UnsupportedFormat, "bad test state");
    std::memcpy(&frame_, bytes, n); return rn::Status::Ok();
  }
  std::string compatId() const override { return "thumbnail-scheduling/1"; }
  std::string buildId() const override { return "test"; }
  uint32_t stateFormatVersion() const override { return 1; }
  rn::CoreKind kind() const override { return rn::CoreKind::External; }
 private:
  uint64_t frame_ = 0;
  std::vector<uint32_t> pixels_ = std::vector<uint32_t>(256 * 240, 0xff123456u);
};

void scheduling(bool shared) {
  rn::registerExternalCore({"thumbnail-scheduling", "thumbnail-scheduling/1",
    uint32_t(rn::CapState | rn::CapRecording | (shared ? uint32_t(rn::CapSharesMachine) : 0u)),
    [] { return std::make_unique<ThumbnailCore>(); }});
  struct Registration { ~Registration() { rn::unregisterExternalCore(); } } registration;
  auto path = std::filesystem::path(RNL_TEST_TMP) / "thumbnail-scheduling.bin";
  { std::ofstream out(path, std::ios::binary); out << "thumbnail core"; }
  rn_session_options options;
  rn_session_options_init(&options);
  options.core = RN_CORE_EXTERNAL;
  rn_session* session = nullptr;
  REQUIRE_EQ(rn_session_new(path.string().c_str(), nullptr, &options, &session), RN_OK);
  std::unique_ptr<rn_input, decltype(&rn_input_free)> input(rn_input_new(), rn_input_free);
  rnl::ThumbnailManager thumbs;
  rnl::EmulationController emu(input.get(), nullptr);
  emu.install(session);
  REQUIRE_EQ(rn_session_core_kind(session), RN_CORE_EXTERNAL);
  CHECK_EQ(bool(rn_core_capabilities(rn_session_core_kind(session)) & RN_CORE_CAP_SHARES_MACHINE), shared);
  for (int i = 0; i < 700; ++i) emu.tick();
  // Install after recording, so all grid pictures must come from renderers.
  emu.observer = &thumbs;
  thumbs.sessionInstalled(session);
  const unsigned hw = std::thread::hardware_concurrency();
  CHECK_EQ(rnl::ThumbnailSchedulingProbe::workers(thumbs),
           size_t(shared ? 1u : std::max(1u, std::min(3u, hw > 2 ? hw - 2 : 1u))));
  const double step = thumbs.layout(1000, 64, rn_take_length(session), 0);
  REQUIRE(emu.coreBusy());
  const int before = creations.load();
  thumbs.pump(session, 1, emu.coreBusy());
  if (shared) CHECK_EQ(creations.load(), before);
  if (shared) CHECK_EQ(rnl::ThumbnailSchedulingProbe::admitted(thumbs), size_t(0));
  else CHECK(rnl::ThumbnailSchedulingProbe::admitted(thumbs) > 0);
  emu.setPaused(true);
  CHECK_FALSE(emu.coreBusy());
  if (shared) {
    // Every paused transport override must still keep background core creation out.
    emu.setRewindHeld(true);
    REQUIRE(emu.coreBusy());
    thumbs.pump(session, 2, emu.coreBusy());
    emu.setRewindHeld(false);
    emu.setFastForwardHeld(true);
    REQUIRE(emu.coreBusy());
    thumbs.pump(session, 3, emu.coreBusy());
    emu.setFastForwardHeld(false);
    emu.pausedStep(-1, true);
    REQUIRE(emu.coreBusy());
    thumbs.pump(session, 4, emu.coreBusy());
    emu.pausedStep(-1, false);
    emu.setStepRepeatHeld(true);
    REQUIRE(emu.coreBusy());
    thumbs.pump(session, 5, emu.coreBusy());
    emu.setStepRepeatHeld(false);
    emu.frameAdvance(2);
    REQUIRE(emu.coreBusy());
    thumbs.pump(session, 6, emu.coreBusy());
    CHECK_EQ(creations.load(), before);
    CHECK_EQ(rnl::ThumbnailSchedulingProbe::admitted(thumbs), size_t(0));
    emu.tick(); emu.tick();
    emu.setRecording(false);
    emu.setPaused(false);
    REQUIRE(emu.coreBusy());
    const int replayBefore = creations.load();
    thumbs.pump(session, 6.1, emu.coreBusy());
    CHECK_EQ(creations.load(), replayBefore);
    CHECK_EQ(rnl::ThumbnailSchedulingProbe::admitted(thumbs), size_t(0));
    emu.setPaused(true);
    emu.practiceSetRange(0, 10, 100);
    emu.setPracticeCountdown(false);
    emu.startPractice(0);
    REQUIRE_EQ(rn_get_mode(session), RN_MODE_PRACTICE);
    REQUIRE(emu.coreBusy());
    const int practiceBefore = creations.load();
    thumbs.pump(session, 6.2, emu.coreBusy());
    CHECK_EQ(creations.load(), practiceBefore);
    CHECK_EQ(rnl::ThumbnailSchedulingProbe::admitted(thumbs), size_t(0));
    emu.setPaused(true); // paused practice must also allow the thumbnails below to finish
  }
  REQUIRE(!emu.coreBusy());
  // A bounded real worker run verifies paused progress and full-batch commit.
  const uint64_t target = rnf_thumb_picture_frame(2, step);
  for (int i = 0; i < 500 && !thumbs.imageAt(target); ++i) {
    thumbs.pump(session, 7 + i * .01, emu.coreBusy());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(bool(thumbs.imageAt(rnf_thumb_picture_frame(1, step))));
  CHECK(bool(thumbs.imageAt(target)));
  if (shared) {
    // Replacing a partly admitted batch must discard it and let the replacement finish.
    thumbs.sessionInstalled(session);
    thumbs.layout(1000, 64, rn_take_length(session), 20);
    const int admissionBefore = creations.load();
    thumbs.pump(session, 21, false);
    REQUIRE_EQ(rnl::ThumbnailSchedulingProbe::admitted(thumbs), size_t(1));
    emu.setPaused(false);
    for (int i = 0; i < 5; ++i) thumbs.pump(session, 21.1 + i * .01, emu.coreBusy());
    CHECK_EQ(rnl::ThumbnailSchedulingProbe::admitted(thumbs), size_t(1));
    CHECK(creations.load() <= admissionBefore + 1);
    emu.setPaused(true);
    thumbs.sessionInstalled(session);
    thumbs.layout(1000, 64, rn_take_length(session), 22);
    for (int i = 0; i < 500 && !thumbs.imageAt(target); ++i) {
      thumbs.pump(session, 23 + i * .01, false);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(bool(thumbs.imageAt(target)));
    auto image = thumbs.imageAt(target);
    if (image) {
      std::ofstream out(std::filesystem::path(RNL_TEST_TMP) / "thumbs-preview.ppm", std::ios::binary);
      out << "P6\n" << image.get()->width << " " << image.get()->height << "\n255\n";
      for (uint32_t pixel : image.get()->px) {
        const char rgb[]{char(pixel >> 16), char(pixel >> 8), char(pixel)};
        out.write(rgb, 3);
      }
    }
  }
  emu.closeSession();
}
}  // namespace

TEST_CASE("shared machine thumbnails wait for paused idle transport") { scheduling(true); }
TEST_CASE("independent machine thumbnails render while transport is busy") { scheduling(false); }
