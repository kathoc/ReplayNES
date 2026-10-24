// Input layouts: a core's own game input (buttons on more bytes, coins, analog axes) through the
// input pipeline (bindings, turbo on any button with its own timing, SOCD, axes), recording, replay
// and projects (inputFormatVersion 2 with the layout in the manifest), and that the NES layout keeps
// writing inputFormatVersion 1.
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "core/ICore.h"
#include "input/InputPipeline.h"
#include "replaynes/product.h"
#include "replaynes/replaynes.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"
#include "util/Bytes.h"
#include "util/Hash.h"
#include "util/Json.h"

using namespace rn;
using namespace rntest;

namespace {

// Two players: directions, three buttons and start on bytes 0/1 (like the NES pads), coins and
// service on byte 3, the stick and the throttle as int16 axes on bytes 4-9.
InputLayout arcadeLayout(const std::string& name = "arcade-test") {
  InputLayout l;
  l.name = name;
  l.recordBytes = 10;
  const char* names[8] = {"up", "down", "left", "right", "b1", "b2", "b3", "start"};
  for (int port = 0; port < 2; ++port) {
    std::string pre = port == 0 ? "p1." : "p2.";
    for (int bit = 0; bit < 8; ++bit) l.buttons.push_back({pre + names[bit], "", uint16_t(port), uint8_t(bit), ""});
    l.buttons.push_back({pre + "coin", "", 3, uint8_t(port), ""});
    l.opposite.push_back({pre + "up", pre + "down"});
    l.opposite.push_back({pre + "left", pre + "right"});
  }
  l.buttons.push_back({"p1.service", "Service", 3, 7, ""});
  l.axes.push_back({"p1.axis.x", "", 4});
  l.axes.push_back({"p1.axis.y", "", 6});
  l.axes.push_back({"p1.axis.throttle", "Throttle", 8});
  l.buttons[4].label = "Shot";
  return l;
}

bool bit(const InputRecord& r, size_t byte, int b) { return (r.byte(byte) >> b) & 1; }
int16_t axis(const InputRecord& r, size_t byte) { return int16_t(r.byte(byte) | r.byte(byte + 1) << 8); }

class ArcadeTestCore final : public ICore {
 public:
  explicit ArcadeTestCore(InputLayout layout) : layout_(std::move(layout)), video_(256 * 240, 0) {}
  Status loadROM(const uint8_t* data, size_t size) override {
    if (!data || !size) return Error(Err::RomInvalid, "empty");
    seed_ = Hasher64::of(data, size);
    loaded_ = true;
    frame_ = 0;
    return powerCycle();
  }
  Status powerCycle() override { acc_ = seed_; return Status::Ok(); }
  Status softReset() override { acc_ ^= 0x77; return Status::Ok(); }
  Status stepFrame(uint8_t p1, uint8_t p2, bool render) override { return stepInput(InputRecord{p1, p2, 0}, render); }
  Status stepInput(const InputRecord& r, bool) override {
    if (!loaded_) return Error(Err::InvalidArg, "no content");
    for (size_t i = 0; i < kInputRecordBytes; ++i)
      if (i != kInputEventsByte) acc_ = (acc_ ^ r.byte(i)) * 1099511628211ull + i;
    for (auto& px : video_) px = 0xFF000000u | uint32_t(acc_ >> 40);
    for (int i = 0; i < 800; ++i) audio_[i] = int16_t(acc_ >> 48);
    ++frame_;
    return Status::Ok();
  }
  uint64_t frameIndex() const override { return frame_; }
  const uint32_t* video() const override { return video_.data(); }
  const int16_t* audio(size_t* count) const override { *count = 800; return audio_; }
  uint64_t samplesBefore(uint64_t f) const override { return f * 800; }
  InputLayout inputLayout() const override { return layout_; }
  Status saveState(std::vector<uint8_t>& out) override {
    ByteWriter w;
    w.u64(acc_);
    w.u64(seed_);
    stateenv::write(out, compatId(), frame_, w.buf.data(), w.buf.size());
    return Status::Ok();
  }
  Status loadState(const uint8_t* data, size_t size) override {
    uint64_t frame;
    const uint8_t* p;
    size_t n;
    RN_TRY(stateenv::read(data, size, compatId(), frame, p, n));
    ByteReader r(p, n);
    acc_ = r.u64();
    seed_ = r.u64();
    if (!r.ok() || r.remaining()) return Error(Err::StateError, "bad state");
    frame_ = frame;
    loaded_ = true;
    return Status::Ok();
  }
  std::string compatId() const override { return "arcade-test/1"; }
  std::string buildId() const override { return "arcade-test"; }
  uint32_t stateFormatVersion() const override { return 1; }
  CoreKind kind() const override { return CoreKind::External; }
  uint64_t accumulator() const { return acc_; }

 private:
  InputLayout layout_;
  bool loaded_ = false;
  uint64_t frame_ = 0, acc_ = 0, seed_ = 0;
  std::vector<uint32_t> video_;
  int16_t audio_[800] = {};
};

}  // namespace

TEST_CASE("input layout: NES layout, validation and JSON round trip") {
  const InputLayout& nes = InputLayout::nes();
  CHECK(nes.isNes());
  CHECK_EQ(nes.recordBytes, 3u);
  CHECK_EQ(nes.buttons.size(), size_t(16));
  CHECK_EQ(nes.turboOf(nes.buttons[0]), std::string("p1.turbo_a"));
  CHECK_EQ(nes.turboOf(nes.buttons[2]), std::string("p1.select.turbo"));
  InputLayout back;
  REQUIRE(InputLayout::fromJson(nes.toJson(), back).ok());
  CHECK(back == nes);

  InputLayout a = arcadeLayout();
  REQUIRE(a.validate().ok());
  REQUIRE(InputLayout::fromJson(a.toJson(), back).ok());
  CHECK(back == a);
  InputLayout bad = a;
  bad.buttons[0].byte = uint16_t(kInputEventsByte);  // the events byte is reserved
  CHECK_FALSE(bad.validate().ok());
  bad = a;
  bad.axes[0].byte = 0;  // overlaps the p1 buttons
  CHECK_FALSE(bad.validate().ok());
  bad = a;
  bad.recordBytes = 4;  // axes beyond the record
  CHECK_FALSE(bad.validate().ok());
}

TEST_CASE("input pipeline: arcade layout bindings, per-button turbo, coins, SOCD and axes") {
  InputPipeline p;
  REQUIRE(p.bind("gc0:buttonA", "p1.a").ok());       // NES actions
  REQUIRE(p.bind("gc0:buttonY", "p1.turbo_a").ok());
  CHECK_FALSE(p.bind("gc0:buttonB", "p1.b2").ok());   // not an NES action
  REQUIRE(p.setLayout(arcadeLayout()).ok());
  CHECK_EQ(p.bindings().size(), size_t(0));  // the arcade layout has neither action
  REQUIRE(p.bind("gc0:buttonA", "p1.b1").ok());
  REQUIRE(p.bind("gc0:buttonX", "p1.b1.turbo").ok());
  REQUIRE(p.bind("gc0:buttonY", "p1.b2.turbo").ok());
  REQUIRE(p.bind("gc0:back", "p1.coin").ok());
  REQUIRE(p.bind("gc0:dpad.up", "p1.up").ok());
  REQUIRE(p.bind("gc0:dpad.down", "p1.down").ok());
  REQUIRE(p.bind("gc0:lstick.x", "p1.axis.x").ok());
  REQUIRE(p.bind("gc0:lstick.-y", "p1.axis.y").ok());
  REQUIRE(p.bind("gc0:rtrigger", "p1.axis.throttle").ok());
  REQUIRE(p.setTurbo(6, 3).ok());
  REQUIRE(p.setButtonTurbo("p1.b1", 4, 1).ok());
  CHECK_FALSE(p.setButtonTurbo("p1.nope", 4, 1).ok());

  // Turbo: b1 with its own timing (on 1 of 4), b2 with the global one (on 3 of 6).
  p.setPressed("gc0:buttonX", true);
  p.setPressed("gc0:buttonY", true);
  std::string b1, b2;
  for (uint64_t f = 100; f < 112; ++f) {
    InputRecord r;
    p.sampleRecord(f, r);
    b1 += bit(r, 0, 4) ? '1' : '0';
    b2 += bit(r, 0, 5) ? '1' : '0';
  }
  CHECK_EQ(b1, std::string("100010001000"));
  CHECK_EQ(b2, std::string("111000111000"));
  p.setPressed("gc0:buttonX", false);
  p.setPressed("gc0:buttonY", false);

  // A coin tap shorter than a frame is latched for one sample.
  p.setPressed("gc0:back", true);
  p.setPressed("gc0:back", false);
  InputRecord r;
  p.sampleRecord(200, r);
  CHECK(bit(r, 3, 0));
  p.sampleRecord(201, r);
  CHECK_FALSE(bit(r, 3, 0));

  // SOCD on the layout's pairs: last pressed wins.
  p.setSocd(Socd::LastWins);
  p.setPressed("gc0:dpad.up", true);
  p.setPressed("gc0:dpad.down", true);
  p.sampleRecord(202, r);
  CHECK(bit(r, 0, 1) && !bit(r, 0, 0));
  p.setPressed("gc0:dpad.up", false);
  p.setPressed("gc0:dpad.down", false);

  // Axes: stick x as is, y inverted (screen down = positive), a digital trigger = full throttle.
  p.setAxis("gc0:lstick", 0.5f, -1.f);
  p.setPressed("gc0:rtrigger", true);
  p.sampleRecord(203, r);
  CHECK_EQ(axis(r, 4), int16_t(16384));
  CHECK_EQ(axis(r, 6), int16_t(32767));
  CHECK_EQ(axis(r, 8), int16_t(32767));
  p.setPressed("gc0:rtrigger", false);
  p.setAxisValue("gc0:rtrigger", 0.25f);
  p.sampleRecord(204, r);
  CHECK_EQ(axis(r, 8), int16_t(8192));
  for (size_t i = 10; i < kInputRecordBytes; ++i) CHECK_EQ(int(r.byte(i)), 0);

  // The configuration round-trips, per-button turbo included.
  InputPipeline q;
  REQUIRE(q.setLayout(arcadeLayout()).ok());
  REQUIRE(q.fromJson(p.toJson()).ok());
  CHECK_EQ(q.toJson(), p.toJson());
  CHECK(p.toJson().find("\"buttonTurbo\"") != std::string::npos);
}

TEST_CASE("input layout: record, replay, save and reopen with a core's own layout") {
  InputLayout layout = arcadeLayout();
  registerExternalCore({"arcade-test", "arcade-test/1", uint32_t(CapAudio | CapState | CapSoftReset | CapRecording),
                        [] { return std::unique_ptr<ICore>(new ArcadeTestCore(arcadeLayout())); }});
  std::string dir = tempDir("input-layout");
  std::string content = writeTestRom(dir, "content.bin");
  rn_session_options opt;
  rn_session_options_init(&opt);
  opt.core = RN_CORE_EXTERNAL;
  rn_session* s = nullptr;
  REQUIRE(rn_session_new(content.c_str(), nullptr, &opt, &s) == RN_OK);
  char* lj = rn_session_input_layout_json(s);
  REQUIRE(lj);
  CHECK_EQ(std::string(lj), layout.toJson());

  // The pipeline is configured from the session's layout, as a frontend does.
  rn_input* in = rn_input_new();
  REQUIRE(rn_input_set_layout_json(in, lj) == RN_OK);
  rn_string_free(lj);
  REQUIRE(rn_input_bind(in, "gc0:buttonA", "p1.b3.turbo") == RN_OK);
  REQUIRE(rn_input_bind(in, "gc0:lstick.x", "p1.axis.x") == RN_OK);
  REQUIRE(rn_input_bind(in, "gc0:back", "p2.coin") == RN_OK);
  REQUIRE(rn_input_set_button_turbo(in, "p1.b3", 3, 1) == RN_OK);

  rn_input_record bad{};
  bad.bytes[12] = 1;  // beyond recordBytes
  CHECK(rn_step_record(s, &bad, nullptr) == RN_ERR_INVALID_ARG);

  std::vector<rn_input_record> applied;
  for (uint64_t f = 0; f < 600; ++f) {
    rn_input_set_pressed(in, "gc0:buttonA", (f / 40) % 2);
    rn_input_set_axis(in, "gc0:lstick", float(int(f % 21) - 10) / 10.f, 0.f);
    if (f % 97 == 5) {
      rn_input_set_pressed(in, "gc0:back", 1);
      rn_input_set_pressed(in, "gc0:back", 0);
    }
    rn_input_record rec;
    rn_input_sample_record(in, f, &rec);
    rn_step_info info{};
    REQUIRE(rn_step_record(s, &rec, &info) == RN_OK);
    REQUIRE(std::memcmp(&info.applied_record, &rec, sizeof rec) == 0);
    applied.push_back(rec);
  }
  rn_input_free(in);

  auto replayApplied = [&](rn_session* session) {
    std::vector<rn_input_record> out;
    REQUIRE(rn_set_mode(session, RN_MODE_REPLAY) == RN_OK);
    REQUIRE(rn_seek(session, 0) == RN_OK);
    for (;;) {
      rn_step_info info{};
      uint64_t before = rn_frame(session);
      REQUIRE(rn_step(session, 0, 0, 0, &info) == RN_OK);
      if (info.frame == before) break;
      out.push_back(info.applied_record);
      if (info.end_of_take) break;
    }
    return out;
  };
  auto same = [&](const std::vector<rn_input_record>& v) {
    return v.size() == applied.size() && std::memcmp(v.data(), applied.data(), v.size() * sizeof(rn_input_record)) == 0;
  };
  CHECK(same(replayApplied(s)));

  std::string project = dir + "/layout" RN_PRODUCT_PROJECT_EXT;
  REQUIRE(rn_session_save_as(s, project.c_str()) == RN_OK);
  rn_session_close(s);
  char* manText = nullptr;
  REQUIRE(rn_project_manifest_json(project.c_str(), &manText) == RN_OK);
  Json man;
  REQUIRE(Json::parse(manText, man));
  rn_string_free(manText);
  CHECK_EQ(man["inputFormatVersion"].asInt(), int64_t(2));
  CHECK_EQ(man["inputLayout"]["name"].asString(), std::string("arcade-test"));
  CHECK_FALSE(man.has("region"));

  REQUIRE(rn_session_open(project.c_str(), content.c_str(), &opt, &s) == RN_OK);
  CHECK(same(replayApplied(s)));
  rn_session_close(s);

  // A core whose layout differs from the project's refuses it.
  registerExternalCore({"arcade-test", "arcade-test/1", uint32_t(CapAudio | CapState | CapSoftReset | CapRecording),
                        [] { return std::unique_ptr<ICore>(new ArcadeTestCore(arcadeLayout("arcade-test-v2"))); }});
  CHECK(rn_session_open(project.c_str(), content.c_str(), &opt, &s) == RN_ERR_CORE_MISMATCH);
  unregisterExternalCore();
}

TEST_CASE("input layout: NES projects keep inputFormatVersion 1 and the NTSC region") {
  std::string dir = tempDir("input-layout-nes");
  std::string rom = writeTestRom(dir);
  rn_session_options opt;
  rn_session_options_init(&opt);
  rn_session* s = nullptr;
  std::string project = dir + "/nes" RN_PRODUCT_PROJECT_EXT;
  REQUIRE(rn_session_new(rom.c_str(), project.c_str(), &opt, &s) == RN_OK);
  for (int f = 0; f < 120; ++f) REQUIRE(rn_step(s, uint8_t(f), 0, 0, nullptr) == RN_OK);
  REQUIRE(rn_session_save(s) == RN_OK);
  rn_session_close(s);
  char* manText = nullptr;
  REQUIRE(rn_project_manifest_json(project.c_str(), &manText) == RN_OK);
  Json man;
  REQUIRE(Json::parse(manText, man));
  rn_string_free(manText);
  CHECK_EQ(man["inputFormatVersion"].asInt(), int64_t(1));
  CHECK_EQ(man["region"].asString(), std::string("NTSC"));
  CHECK_FALSE(man.has("inputLayout"));
}
