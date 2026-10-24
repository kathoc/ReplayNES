// Media info for cores that are not the NES (rn_media_info, rn_session_video_frame,
// rn_session_samples_before, the renderer equivalents) and the external core registration, through
// the public C API with a test core whose picture size changes, whose rows are padded, whose sound
// is stereo with a constant number of sample frames per frame and whose frame rate is 59.63 Hz.
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/ICore.h"
#include "replaynes/product.h"
#include "replaynes/replaynes.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"
#include "util/Bytes.h"
#include "util/Hash.h"

using namespace rn;
using namespace rntest;

namespace {

constexpr uint32_t kMaxW = 384, kMaxH = 256, kSpf = 805;
constexpr const char* kCompat = "wide-stereo-test/1";

// 320x224 normally, 288x224 for frames [1000, 2000) of every 3000: the size follows the frame index
// so seeking and replaying reproduce it.
uint32_t widthAt(uint64_t frame) { return (frame % 3000) >= 1000 && (frame % 3000) < 2000 ? 288 : 320; }

class WideStereoCore final : public ICore {
 public:
  WideStereoCore() : video_(size_t(kMaxW) * kMaxH, 0), audio_(kSpf * 2, 0) {}
  Status loadROM(const uint8_t* data, size_t size) override {
    if (!data || !size) return Error(Err::RomInvalid, "empty");
    seed_ = Hasher64::of(data, size);
    loaded_ = true;
    frame_ = 0;
    return powerCycle();
  }
  Status powerCycle() override { acc_ = seed_; return Status::Ok(); }
  Status softReset() override { acc_ ^= 0x5555; return Status::Ok(); }
  // Like an arcade set: the content is named by its file name, whatever bytes the file holds.
  Status loadContent(const std::string& path, const uint8_t* data, size_t size) override {
    size_t slash = path.find_last_of("/\\");
    std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    id_ = "set:" + name.substr(0, name.find('.'));
    const uint8_t* named = reinterpret_cast<const uint8_t*>(id_.data());
    (void)data;
    (void)size;
    return loadROM(named, id_.size());
  }
  std::string contentId() const override { return id_; }
  Status stepFrame(uint8_t p1, uint8_t p2, bool renderVideo) override {
    if (!loaded_) return Error(Err::InvalidArg, "no content");
    acc_ = acc_ * 6364136223846793005ULL + 1442695040888963407ULL ^ (uint64_t(p1) << 8 | uint64_t(p2) << 16);
    width_ = widthAt(frame_);
    if (renderVideo)
      for (uint32_t y = 0; y < 224; ++y)
        for (uint32_t x = 0; x < kMaxW; ++x)  // padding columns too: they must not reach any hash
          video_[size_t(y) * kMaxW + x] = x < width_ ? 0xFF000000u | uint32_t(acc_ >> 40) ^ (x * 31 + y) : 0xDEADBEEF ^ uint32_t(frame_);
    for (uint32_t i = 0; i < kSpf * 2; ++i) audio_[i] = int16_t((acc_ >> 32) + i * (i & 1 ? 7 : 13));
    count_ = kSpf;
    ++frame_;
    return Status::Ok();
  }
  uint64_t frameIndex() const override { return frame_; }
  const uint32_t* video() const override { return video_.data(); }
  const int16_t* audio(size_t* count) const override { *count = count_; return audio_.data(); }
  MediaInfo mediaInfo() const override {
    MediaInfo m;
    m.maxWidth = kMaxW;
    m.maxHeight = kMaxH;
    m.fpsNum = 5963;
    m.fpsDen = 100;
    m.audioChannels = 2;
    m.pixelAspectNum = m.pixelAspectDen = 1;
    m.rotation = 1;
    return m;
  }
  VideoFrame videoFrame() const override { return VideoFrame{video_.data(), width_, 224, kMaxW}; }
  uint64_t samplesBefore(uint64_t frame) const override { return frame * kSpf; }
  Status saveState(std::vector<uint8_t>& out) override {
    ByteWriter w;
    w.u64(acc_);
    w.u64(seed_);
    w.u32(width_);
    stateenv::write(out, kCompat, frame_, w.buf.data(), w.buf.size());
    return Status::Ok();
  }
  Status loadState(const uint8_t* data, size_t size) override {
    uint64_t frame;
    const uint8_t* p;
    size_t n;
    RN_TRY(stateenv::read(data, size, kCompat, frame, p, n));
    ByteReader r(p, n);
    acc_ = r.u64();
    seed_ = r.u64();
    width_ = r.u32();
    if (!r.ok() || r.remaining()) return Error(Err::StateError, "bad state");
    frame_ = frame;
    loaded_ = true;
    count_ = 0;
    return Status::Ok();
  }
  std::string compatId() const override { return kCompat; }
  std::string buildId() const override { return "wide-stereo-test"; }
  uint32_t stateFormatVersion() const override { return 1; }
  CoreKind kind() const override { return CoreKind::External; }

 private:
  bool loaded_ = false;
  uint64_t frame_ = 0, acc_ = 0, seed_ = 0;
  std::string id_;
  uint32_t width_ = 320;
  std::vector<uint32_t> video_;
  std::vector<int16_t> audio_;
  size_t count_ = 0;
};

struct Frame {
  uint32_t width;
  uint64_t video, audio;
  bool operator==(const Frame& o) const { return width == o.width && video == o.video && audio == o.audio; }
};

// Visible rows only, so the padding (which changes every frame) cannot leak into the comparison.
uint64_t pictureHash(const rn_video_frame_info& v) {
  Hasher64 h;
  for (uint32_t y = 0; y < v.height; ++y) h.update(v.pixels + size_t(y) * v.stride, size_t(v.width) * 4);
  return h.digest();
}

Frame sessionFrame(const rn_session* s) {
  rn_video_frame_info v{};
  REQUIRE(rn_session_video_frame(s, &v) == RN_OK);
  size_t n = 0;
  const int16_t* a = rn_audio(s, &n);
  return {v.width, pictureHash(v), Hasher64::of(a, n * 2 * sizeof(int16_t), n)};
}

std::vector<Frame> replay(rn_session* s) {
  REQUIRE(rn_set_mode(s, RN_MODE_REPLAY) == RN_OK);
  REQUIRE(rn_seek(s, 0) == RN_OK);
  std::vector<Frame> out;
  for (;;) {
    rn_step_info info{};
    uint64_t before = rn_frame(s);
    REQUIRE(rn_step(s, 0, 0, 0, &info) == RN_OK);
    if (info.frame == before) break;
    out.push_back(sessionFrame(s));
    if (info.end_of_take) break;
  }
  return out;
}

}  // namespace

TEST_CASE("media: an external core with its own size, rate and stereo sound, end to end") {
  CHECK_FALSE(rn_core_available(RN_CORE_EXTERNAL));
  registerExternalCore({"wide-stereo-test", kCompat, uint32_t(CapAudio | CapState | CapSoftReset | CapRecording),
                        [] { return std::unique_ptr<ICore>(new WideStereoCore()); }});
  REQUIRE(rn_core_available(RN_CORE_EXTERNAL));
  CHECK_EQ(std::string(rn_core_name(RN_CORE_EXTERNAL)), std::string("wide-stereo-test"));

  std::string dir = tempDir("media");
  std::string content = writeTestRom(dir, "content.bin");
  rn_session_options opt;
  rn_session_options_init(&opt);
  opt.core = RN_CORE_EXTERNAL;
  rn_session* s = nullptr;
  REQUIRE(rn_session_new(content.c_str(), nullptr, &opt, &s) == RN_OK);
  REQUIRE(rn_session_core_kind(s) == RN_CORE_EXTERNAL);

  rn_media_info m{};
  REQUIRE(rn_session_media_info(s, &m) == RN_OK);
  CHECK(m.max_width == kMaxW && m.max_height == kMaxH);
  CHECK(m.fps_num == 5963 && m.fps_den == 100);
  CHECK(m.audio_channels == 2);
  CHECK(m.pixel_aspect_num == 1 && m.pixel_aspect_den == 1);
  CHECK(m.rotation == 1);
  for (uint64_t f : {0ull, 1ull, 59ull, 3001ull}) CHECK_EQ(rn_session_samples_before(s, f), f * kSpf);

  // Record 2500 frames: the picture is 320 wide, 288 wide from frame 1000, 320 again from 2000.
  std::vector<Frame> recorded;
  for (uint64_t f = 0; f < 2500; ++f) {
    REQUIRE(rn_step(s, uint8_t(f * 7), uint8_t(f >> 3), f == 1500 ? RN_EV_SOFT_RESET : 0, nullptr) == RN_OK);
    recorded.push_back(sessionFrame(s));
    rn_video_frame_info v{};
    REQUIRE(rn_session_video_frame(s, &v) == RN_OK);
    if (v.width != widthAt(f) || v.height != 224 || v.stride != kMaxW) FAIL("picture size at frame " << f);
    size_t n = 0;
    rn_audio(s, &n);
    if (n != kSpf) FAIL("sample frames at frame " << f << ": " << n);
  }

  // Replay, then save + reopen (the external core is found by its compat ID), then export.
  CHECK(replay(s) == recorded);
  std::string project = dir + "/media" RN_PRODUCT_PROJECT_EXT;
  REQUIRE(rn_session_save_as(s, project.c_str()) == RN_OK);
  rn_session_close(s);
  REQUIRE(rn_session_open(project.c_str(), content.c_str(), &opt, &s) == RN_OK);
  REQUIRE(rn_session_core_kind(s) == RN_CORE_EXTERNAL);
  CHECK(replay(s) == recorded);

  rn_renderer* r = nullptr;
  REQUIRE(rn_renderer_new(s, 0, 0, &r) == RN_OK);
  rn_media_info rm{};
  REQUIRE(rn_renderer_media_info(r, &rm) == RN_OK);
  CHECK(std::memcmp(&rm, &m, sizeof m) == 0);
  std::vector<Frame> rendered;
  for (;;) {
    const uint32_t* video = nullptr;
    const int16_t* audio = nullptr;
    size_t n = 0;
    uint64_t index = 0;
    rn_status st = rn_renderer_next(r, &video, &audio, &n, &index);
    if (st == RN_ERR_END_OF_TAKE) break;
    REQUIRE(st == RN_OK);
    rn_video_frame_info v{};
    REQUIRE(rn_renderer_video_frame(r, &v) == RN_OK);
    REQUIRE(v.pixels == video);
    rendered.push_back({v.width, pictureHash(v), Hasher64::of(audio, n * 2 * sizeof(int16_t), n)});
  }
  rn_renderer_free(r);
  rn_session_close(s);
  CHECK(rendered == recorded);

  // Content identity: the manifest names the content, not the file's hash; a file with other bytes
  // but the same name opens (a repacked set), another name is refused.
  char* manText = nullptr;
  REQUIRE(rn_project_manifest_json(project.c_str(), &manText) == RN_OK);
  std::string man(manText);
  rn_string_free(manText);
  CHECK(man.find("\"identity\": \"content\"") != std::string::npos);
  CHECK(man.find("set:content") != std::string::npos);
  std::string other = tempDir("media-repacked");
  std::string repacked = writeTestRom(other, "content.bin");
  { std::FILE* f = std::fopen(repacked.c_str(), "ab"); REQUIRE(f); std::fputs("other bytes", f); std::fclose(f); }
  REQUIRE(rn_session_open(project.c_str(), repacked.c_str(), &opt, &s) == RN_OK);
  CHECK(replay(s) == recorded);
  rn_session_close(s);
  std::string renamed = writeTestRom(other, "another.bin");
  CHECK(rn_session_open(project.c_str(), renamed.c_str(), &opt, &s) == RN_ERR_ROM_MISMATCH);

  unregisterExternalCore();
  CHECK_FALSE(rn_core_available(RN_CORE_EXTERNAL));
}
