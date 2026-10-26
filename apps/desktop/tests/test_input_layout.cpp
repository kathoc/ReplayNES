// SPDX-License-Identifier: GPL-2.0-or-later
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include "emulation.h"
#include "diagram_navigation.h"
#include "replaynes/frontend.h"
#include "replaynes/product.h"
#include "support/rn_test.h"

#include <algorithm>
#include <cstring>
#include "core/ICore.h"

namespace s5b {
inline rn::InputLayout layout() {
  rn::InputLayout l;
  l.name = "s5b-arcade";
  l.recordBytes = 9;
  for (int player = 1; player <= 2; ++player) {
    const std::string p = "p" + std::to_string(player) + ".";
    const uint16_t b = player == 1 ? 0 : 1;
    l.buttons.push_back({p + "b1", "Fire", b, 0});
    l.buttons.push_back({p + "b2", "", b, 1});
    l.buttons.push_back({p + "b3", "", b, 2});
    l.buttons.push_back({p + "coin", "", b, 3});
    l.buttons.push_back({p + "start", "", b, 4});
    // Deliberately different from NES direction bit positions.
    l.buttons.push_back({p + "up", "", b, 5});
    l.buttons.push_back({p + "down", "", b, 6});
    l.buttons.push_back({p + "left", "", b, 7});
    l.buttons.push_back({p + "right", "", 3, uint8_t(player - 1)});
    l.opposite.push_back({p + "up", p + "down"});
    l.opposite.push_back({p + "left", p + "right"});
  }
  l.buttons.push_back({"service", "Service", 4, 0});
  l.axes.push_back({"p1.axis.x", "", 5});
  l.axes.push_back({"p2.axis.y", "Vertical", 7});
  return l;
}

class Core final : public rn::ICore {
 public:
  rn::Status loadROM(const uint8_t*, size_t) override { frame_ = 0; return powerCycle(); }
  rn::Status powerCycle() override { input_ = {}; return rn::Status::Ok(); }
  rn::Status softReset() override { return powerCycle(); }
  rn::Status stepFrame(uint8_t p1, uint8_t p2, bool render) override {
    return stepInput({p1, p2, 0}, render);
  }
  rn::Status stepInput(const rn::InputRecord& in, bool render) override {
    input_ = in;
    ++frame_;
    if (render) for (size_t i = 0; i < pixels_.size(); ++i)
      pixels_[i] = 0xff000000u | in.byte(i % 9);
    return rn::Status::Ok();
  }
  rn::InputLayout inputLayout() const override { return layout(); }
  rn::MediaInfo mediaInfo() const override { return {64, 64, 50, 1, 1, 1, 1, 0}; }
  rn::VideoFrame videoFrame() const override { return {pixels_.data(), 64, 64, 64}; }
  uint64_t frameIndex() const override { return frame_; }
  const uint32_t* video() const override { return pixels_.data(); }
  const int16_t* audio(size_t* count) const override { *count = 0; return nullptr; }
  uint64_t samplesBefore(uint64_t) const override { return 0; }
  rn::Status saveState(std::vector<uint8_t>& out) override {
    out.resize(sizeof(frame_) + sizeof(input_));
    std::memcpy(out.data(), &frame_, sizeof(frame_));
    std::memcpy(out.data() + sizeof(frame_), &input_, sizeof(input_));
    return rn::Status::Ok();
  }
  rn::Status loadState(const uint8_t* in, size_t size) override {
    if (size != sizeof(frame_) + sizeof(input_)) return rn::Error(rn::Err::Corrupt, "state size");
    std::memcpy(&frame_, in, sizeof(frame_));
    std::memcpy(&input_, in + sizeof(frame_), sizeof(input_));
    return rn::Status::Ok();
  }
  std::string compatId() const override { return "s5b-arcade/1"; }
  std::string buildId() const override { return "test"; }
  std::string contentId() const override { return "s5b-test-set"; }
  uint32_t stateFormatVersion() const override { return 1; }
  rn::CoreKind kind() const override { return rn::CoreKind::External; }
 private:
  uint64_t frame_ = 0;
  rn::InputRecord input_{};
  std::vector<uint32_t> pixels_ = std::vector<uint32_t>(64 * 64, 0xff000000);
};
inline void registerCore() {
  rn::registerExternalCore({"s5b-arcade", "s5b-arcade/1", rn::CapState | rn::CapRecording | rn::CapSoftReset,
                            [] { return std::make_unique<Core>(); }});
}
}

namespace {
std::string take(char* text) {
  std::string result = text ? text : "";
  rn_string_free(text);
  return result;
}
struct LayoutScope {
  ~LayoutScope() { rnf_input_set_layout_json(nullptr); rn::unregisterExternalCore(); }
};
std::map<std::string, rnf_action_group> catalog() {
  std::map<std::string, rnf_action_group> result;
  for (size_t i = 0; i < rnf_input_action_count(); ++i) {
    rnf_action_info a{};
    REQUIRE(rnf_input_action_get(i, &a));
    REQUIRE(result.emplace(a.id, a.group).second);
  }
  return result;
}
std::set<std::string> defaults() {
  std::set<std::string> result;
  for (size_t i = 0; i < rnf_input_default_binding_count(RNF_KEYBOARD_SDL); ++i) {
    rnf_binding b{};
    REQUIRE(rnf_input_default_binding_get(RNF_KEYBOARD_SDL, i, &b));
    result.insert(std::string(b.input) + "=" + b.action);
  }
  return result;
}
std::string readFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}

TEST_CASE("S5b catalog and defaults follow the external session layout") {
  LayoutScope scope;
  s5b::registerCore();
  const auto path = std::filesystem::path(RNL_TEST_TMP) / "s5b-catalog.bin";
  { std::ofstream out(path); out << "arcade content"; }
  rn_session_options options;
  rn_session_options_init(&options);
  options.core = RN_CORE_EXTERNAL;
  rn_session* raw = nullptr;
  REQUIRE_EQ(rn_session_new(path.string().c_str(), nullptr, &options, &raw), RN_OK);
  std::unique_ptr<rn_session, decltype(&rn_session_close)> session(raw, rn_session_close);
  const std::string json = take(rn_session_input_layout_json(session.get()));
  REQUIRE_EQ(rnf_input_set_layout_json(json.c_str()), RN_OK);
  CHECK_EQ(std::string(rnf_input_layout_name()), "s5b-arcade");
  auto actions = catalog();
  const auto layout = s5b::layout();
  CHECK_EQ(actions.size(), layout.buttons.size() * 2 + layout.axes.size() + 13);
  for (const auto& b : layout.buttons) {
    REQUIRE(actions.count(b.id));
    REQUIRE(actions.count(layout.turboOf(b)));
    auto expected = b.id.find("p1.") == 0 ? RNF_GROUP_PLAYER1 :
                    b.id.find("p2.") == 0 ? RNF_GROUP_PLAYER2 : RNF_GROUP_SYSTEM;
    CHECK_EQ(actions.at(b.id), expected);
    CHECK_EQ(actions.at(layout.turboOf(b)), expected);
  }
  CHECK_EQ(take(rnf_input_action_label("p1.b1")), "Fire");
  CHECK_EQ(take(rnf_input_action_label("p1.b1.turbo")), "Fire (Turbo)");
  CHECK_EQ(take(rnf_input_action_label("p2.b2")), "Button 2");
  CHECK_EQ(take(rnf_input_action_label("p1.coin")), "Coin");
  CHECK_EQ(take(rnf_input_action_label("p1.up")), "Up");
  CHECK_EQ(take(rnf_input_action_label("p2.axis.y")), "Vertical");
  CHECK_EQ(actions.at("p1.axis.x"), RNF_GROUP_PLAYER1);
  auto bindings = defaults();
  for (int slot = 0; slot < 2; ++slot) {
    const std::string gc = "gc" + std::to_string(slot) + ":";
    const std::string p = "p" + std::to_string(slot + 1) + ".";
    CHECK(bindings.count(gc + "face.south=" + p + "b1"));
    CHECK(bindings.count(gc + "face.east=" + p + "b2"));
    CHECK(bindings.count(gc + "face.west=" + p + "b3"));
    CHECK(bindings.count(gc + "face.north=" + p + "b1.turbo"));
    CHECK(bindings.count(gc + "options=" + p + "coin"));
    CHECK(bindings.count(gc + "menu=" + p + "start"));
    for (const char* dir : {"up", "down", "left", "right"}) {
      CHECK(bindings.count(gc + "dpad." + dir + "=" + p + dir));
      CHECK(bindings.count(gc + "lstick." + dir + "=" + p + dir));
    }
  }
  CHECK(bindings.count("gc0:lstick.x=p1.axis.x"));
  CHECK(bindings.count("gc1:lstick.-y=p2.axis.y"));
  CHECK_FALSE(actions.count("p1.a"));
  std::filesystem::remove(path);
}

TEST_CASE("Numbered buttons with gaps take the face buttons in order") {
  LayoutScope scope;
  rn::InputLayout l;
  l.name = "gaps";
  l.recordBytes = 5;
  for (int player = 1; player <= 2; ++player) {
    const std::string p = "p" + std::to_string(player) + ".";
    const uint16_t byte = player == 1 ? 0 : 1;
    // Galaxy Force II: shoot, missile, brake (b7), accelerator (b8), listed out of order.
    l.buttons.push_back({p + "b8", "Accelerate", byte, 0});
    l.buttons.push_back({p + "b1", "Shoot", byte, 1});
    l.buttons.push_back({p + "b7", "Brake", byte, 2});
    l.buttons.push_back({p + "b2", "Missile", byte, 3});
    l.buttons.push_back({p + "b9", "", byte, 4});
    l.buttons.push_back({p + "b10", "", byte, 5});
    l.buttons.push_back({p + "b11", "", byte, 6});
  }
  REQUIRE_EQ(rnf_input_set_layout_json(l.toJson().c_str()), RN_OK);
  auto bindings = defaults();
  for (int slot = 0; slot < 2; ++slot) {
    const std::string gc = "gc" + std::to_string(slot) + ":";
    const std::string p = "p" + std::to_string(slot + 1) + ".";
    CHECK(bindings.count(gc + "face.south=" + p + "b1"));
    CHECK(bindings.count(gc + "face.east=" + p + "b2"));
    CHECK(bindings.count(gc + "face.west=" + p + "b7"));
    CHECK(bindings.count(gc + "face.north=" + p + "b8"));
  }
  // Player 1's shoulders stay hotkeys; player 2's take the next buttons (b10 after b9, not before).
  for (const auto& b : bindings) CHECK_FALSE(b.rfind("gc0:leftShoulder=p1.", 0) == 0);
  for (const auto& b : bindings) CHECK_FALSE(b.rfind("gc0:rightShoulder=p1.", 0) == 0);
  CHECK(bindings.count("gc1:leftShoulder=p2.b9"));
  CHECK(bindings.count("gc1:rightShoulder=p2.b10"));
  for (const auto& b : bindings) CHECK(b.find("=p2.b11") == std::string::npos);
}

TEST_CASE("Spare face buttons take the turbo of the numbered buttons") {
  LayoutScope scope;
  for (int count = 1; count <= 2; ++count) {
    rn::InputLayout l;
    l.name = "spare";
    l.recordBytes = 5;
    for (int i = 1; i <= count; ++i) l.buttons.push_back({"p1.b" + std::to_string(i), "", 0, uint8_t(i)});
    REQUIRE_EQ(rnf_input_set_layout_json(l.toJson().c_str()), RN_OK);
    auto bindings = defaults();
    CHECK(bindings.count("gc0:face.south=p1.b1"));
    if (count == 1) {
      CHECK(bindings.count("gc0:face.east=p1.b1.turbo"));
      for (const auto& b : bindings) CHECK(b.rfind("gc0:face.west=", 0) != 0 && b.rfind("gc0:face.north=", 0) != 0);
    } else {
      CHECK(bindings.count("gc0:face.east=p1.b2"));
      CHECK(bindings.count("gc0:face.west=p1.b1.turbo"));
      CHECK(bindings.count("gc0:face.north=p1.b2.turbo"));
    }
  }
}

TEST_CASE("S5b NES catalog defaults and bindings filename survive layout switching exactly") {
  LayoutScope scope;
  REQUIRE_EQ(rnf_input_set_layout_json(nullptr), RN_OK);
  const std::string nesDefaults = take(rnf_input_default_config_json(RNF_KEYBOARD_SDL));
  std::vector<std::string> nesActions;
  for (size_t i = 0; i < rnf_input_action_count(); ++i) {
    rnf_action_info a{}; REQUIRE(rnf_input_action_get(i, &a));
    nesActions.push_back(std::string(a.id) + "=" + take(rnf_input_action_label(a.id)));
  }
  REQUIRE_EQ(nesActions.size(), size_t(33));
  CHECK_EQ(nesActions[0], "p1.up=↑ Up");
  CHECK_EQ(nesActions[8], "p1.turbo_a=Turbo A");
  CHECK_EQ(nesActions[10], "p2.up=↑ Up");
  CHECK_EQ(take(rnf_input_bindings_filename()), "bindings.json");
  REQUIRE_EQ(rnf_input_set_layout_json(s5b::layout().toJson().c_str()), RN_OK);
  CHECK_EQ(take(rnf_input_bindings_filename()), "bindings-s5b-arcade.json");
  REQUIRE_EQ(rnf_input_set_layout_json(rn::InputLayout::nes().toJson().c_str()), RN_OK);
  CHECK_EQ(take(rnf_input_bindings_filename()), "bindings.json");
  CHECK_EQ(take(rnf_input_default_config_json(RNF_KEYBOARD_SDL)), nesDefaults);
  for (size_t i = 0; i < nesActions.size(); ++i) {
    rnf_action_info a{}; REQUIRE(rnf_input_action_get(i, &a));
    CHECK_EQ(std::string(a.id) + "=" + take(rnf_input_action_label(a.id)), nesActions[i]);
  }
}

TEST_CASE("S5b per-layout binding files persist global and per-button Turbo page model") {
  LayoutScope scope;
  const auto root = std::filesystem::path(RNL_TEST_TMP) / "s5b-binding-files";
  std::filesystem::create_directories(root);
  REQUIRE_EQ(rnf_input_set_layout_json(nullptr), RN_OK);
  const auto nesPath = root / take(rnf_input_bindings_filename());
  const std::string nes = take(rnf_input_default_config_json(RNF_KEYBOARD_SDL));
  { std::ofstream out(nesPath); out << nes; }
  auto layout = s5b::layout();
  REQUIRE_EQ(rnf_input_set_layout_json(layout.toJson().c_str()), RN_OK);
  const auto arcadePath = root / take(rnf_input_bindings_filename());
  REQUIRE(arcadePath != nesPath);
  std::unique_ptr<rn_input, decltype(&rn_input_free)> input(rn_input_new(), rn_input_free);
  REQUIRE_EQ(rn_input_set_layout_json(input.get(), layout.toJson().c_str()), RN_OK);
  REQUIRE_EQ(rn_input_load_json(input.get(), take(rnf_input_default_config_json(RNF_KEYBOARD_SDL)).c_str()), RN_OK);
  rn_input_set_turbo(input.get(), 5, 2);
  REQUIRE_EQ(rn_input_set_button_turbo(input.get(), "p1.b1", 10, 3), RN_OK);
  { std::ofstream out(arcadePath); out << take(rn_input_save_json(input.get())); }
  CHECK_EQ(readFile(nesPath), nes);
  rnf_input_config* raw = nullptr;
  REQUIRE_EQ(rnf_input_config_parse(readFile(arcadePath).c_str(), &raw), RN_OK);
  std::unique_ptr<rnf_input_config, decltype(&rnf_input_config_free)> config(raw, rnf_input_config_free);
  CHECK_EQ(rnf_input_config_turbo_period(config.get()), 5);
  CHECK_EQ(rnf_input_config_turbo_duty(config.get()), 2);
  int period = 0, duty = 0;
  REQUIRE(rnf_input_config_button_turbo(config.get(), "p1.b1", &period, &duty));
  CHECK_EQ(period, 10); CHECK_EQ(duty, 3);
  CHECK_FALSE(rnf_input_config_button_turbo(config.get(), "p1.b2", &period, &duty));
  CHECK_EQ(rnf_input_button_count(), layout.buttons.size());
  for (size_t i = 0; i < layout.buttons.size(); ++i)
    CHECK_EQ(std::string(rnf_input_button_id(i)), layout.buttons[i].id);
  const std::string rate50 = take(rnf_input_turbo_rate_label(50.0, 10));
  const std::string rate60 = take(rnf_input_turbo_rate_label(60.0, 10));
  CHECK(rate50.find("5") != std::string::npos);
  CHECK(rate60.find("6") != std::string::npos);
  CHECK(rate50 != rate60);
  REQUIRE_EQ(rn_input_load_json(input.get(), readFile(arcadePath).c_str()), RN_OK);
  REQUIRE_EQ(rn_input_bind(input.get(), "test:turbo", "p1.b1.turbo"), RN_OK);
  rn_input_set_pressed(input.get(), "test:turbo", 1);
  for (uint64_t frame = 0; frame < 20; ++frame) {
    rn_input_record record{};
    rn_input_sample_record(input.get(), frame, &record);
    CHECK_EQ(record.bytes[0] & 1, frame % 10 < 3 ? 1 : 0);
  }
  std::filesystem::remove_all(root);
}

TEST_CASE("S5b desktop stepping preserves service analog axes and layout direction ids") {
  LayoutScope scope;
  s5b::registerCore();
  const auto path = std::filesystem::path(RNL_TEST_TMP) / "s5b-step.bin";
  { std::ofstream out(path); out << "arcade content"; }
  rn_session_options options;
  rn_session_options_init(&options); options.core = RN_CORE_EXTERNAL;
  rn_session* session = nullptr;
  REQUIRE_EQ(rn_session_new(path.string().c_str(), nullptr, &options, &session), RN_OK);
  std::unique_ptr<rn_input, decltype(&rn_input_free)> input(rn_input_new(), rn_input_free);
  rnl::EmulationController emu(input.get(), nullptr);
  emu.install(session);
  emu.setCrtDisplay(false);
  emu.setFlashLevel(RN_FLASH_OFF);
  REQUIRE_EQ(rn_input_bind(input.get(), "test:service", "service"), RN_OK);
  REQUIRE_EQ(rn_input_bind(input.get(), "test:right", "p1.right"), RN_OK);
  REQUIRE_EQ(rn_input_bind(input.get(), "test:coin", "p2.coin"), RN_OK);
  REQUIRE_EQ(rn_input_bind(input.get(), "test:x", "p1.axis.x"), RN_OK);
  REQUIRE_EQ(rn_input_bind(input.get(), "test:y", "p2.axis.y"), RN_OK);
  rn_input_set_pressed(input.get(), "test:service", 1);
  rn_input_set_pressed(input.get(), "test:right", 1);
  rn_input_set_pressed(input.get(), "test:coin", 1);
  rn_input_set_axis_value(input.get(), "test:x", 1.0f);
  rn_input_set_axis_value(input.get(), "test:y", -1.0f);
  rn_input_record record{};
  rn_input_sample_record(input.get(), 0, &record);
  CHECK_EQ(rnf_input_record_directions(&record), RN_BTN_RIGHT);
  CHECK_EQ(record.bytes[0], 0);
  CHECK_EQ(record.bytes[1], 8);
  CHECK_EQ(record.bytes[3], 1);
  CHECK_EQ(record.bytes[4], 1);
  emu.setPaused(false);
  REQUIRE(emu.tick().emulated);
  const auto picture = emu.videoFrame();
  REQUIRE(picture.pixels);
  for (size_t i = 0; i < 9; ++i) CHECK_EQ(picture.pixels[i], 0xff000000u | record.bytes[i]);
  CHECK(record.bytes[5] || record.bytes[6]);
  CHECK(record.bytes[7] || record.bytes[8]);
  emu.closeSession();
  std::filesystem::remove(path);
}

TEST_CASE("S5b countdown buffers layout buttons without latching analog axes or events") {
  LayoutScope scope;
  REQUIRE_EQ(rnf_input_set_layout_json(s5b::layout().toJson().c_str()), RN_OK);
  std::unique_ptr<rnf_resume_countdown, decltype(&rnf_resume_countdown_free)>
      countdown(rnf_resume_countdown_new(), rnf_resume_countdown_free);
  REQUIRE(rnf_resume_countdown_start(countdown.get(), 0, RN_MODE_RECORD, RNF_PHASE_PLAYING));
  rn_input_record blank{}, tap{};
  rnf_resume_countdown_input_record(countdown.get(), &blank);
  tap.bytes[0] = 0x80; // Player 1 left, deliberately not the NES left bit.
  tap.bytes[4] = 1; // Service.
  tap.bytes[2] = RN_EV_SOFT_RESET;
  tap.bytes[5] = 123; tap.bytes[6] = 45;
  rnf_resume_countdown_input_record(countdown.get(), &tap);
  rnf_resume_countdown_input_record(countdown.get(), &blank);
  REQUIRE(rnf_resume_countdown_tick(countdown.get(), 3.0).done);
  rn_input_record live{};
  live.bytes[3] = 1; // Holding right wins over the buffered left tap.
  live.bytes[5] = 7; live.bytes[6] = 8;
  REQUIRE(rnf_resume_countdown_apply_record(countdown.get(), &live));
  CHECK_EQ(live.bytes[0], 0);
  CHECK_EQ(live.bytes[3], 1);
  CHECK_EQ(live.bytes[4], 1);
  CHECK_EQ(live.bytes[2], 0);
  CHECK_EQ(live.bytes[5], 7);
  CHECK_EQ(live.bytes[6], 8);
  rn_input_record next{};
  CHECK_FALSE(rnf_resume_countdown_apply_record(countdown.get(), &next));
  CHECK_EQ(next.bytes[4], 0);
}

TEST_CASE("S5b paused transport and retry preserve the complete generic record") {
  LayoutScope scope;
  s5b::registerCore();
  const auto path = std::filesystem::path(RNL_TEST_TMP) / "s5b-transport.bin";
  { std::ofstream out(path); out << "arcade content"; }
  rn_session_options options;
  rn_session_options_init(&options); options.core = RN_CORE_EXTERNAL;
  rn_session* raw = nullptr;
  REQUIRE_EQ(rn_session_new(path.string().c_str(), nullptr, &options, &raw), RN_OK);
  std::unique_ptr<rn_session, decltype(&rn_session_close)> session(raw, rn_session_close);
  std::unique_ptr<rnf_retry, decltype(&rnf_retry_free)> retry(rnf_retry_new(), rnf_retry_free);
  rn_input_record original{}, replacement{};
  original.bytes[4] = 1; original.bytes[5] = 0x34; original.bytes[6] = 0x12;
  replacement.bytes[1] = 8; replacement.bytes[7] = 0x76; replacement.bytes[8] = 0x54;
  rn_step_info info{};
  for (int i = 0; i < 4; ++i)
    REQUIRE_EQ(rnf_retry_step_record(retry.get(), session.get(), i * 0.02, &original, &info), RN_OK);
  REQUIRE_EQ(rn_seek(session.get(), 1), RN_OK);
  const size_t takes = rn_take_count(session.get());
  REQUIRE_EQ(rnf_transport_step_record(session.get(), &replacement, &info), RN_OK);
  CHECK_EQ(rn_take_count(session.get()), takes);
  for (size_t i = 0; i < 9; ++i) CHECK_EQ(info.applied_record.bytes[i], original.bytes[i]);
  REQUIRE_EQ(rn_seek(session.get(), 4), RN_OK);
  REQUIRE_EQ(rnf_transport_step_record(session.get(), &replacement, &info), RN_OK);
  for (size_t i = 0; i < 9; ++i) CHECK_EQ(info.applied_record.bytes[i], replacement.bytes[i]);
  REQUIRE_EQ(rn_seek(session.get(), 2), RN_OK);
  REQUIRE_EQ(rnf_retry_step_record(retry.get(), session.get(), 1, &replacement, &info), RN_OK);
  CHECK(info.branched);
  for (size_t i = 0; i < 9; ++i) CHECK_EQ(info.applied_record.bytes[i], replacement.bytes[i]);
  CHECK_EQ(rn_take_count(session.get()), takes + 1);
  std::filesystem::remove(path);
}

TEST_CASE("S5b Turbo settings page enumerates every layout button and global controls") {
  LayoutScope scope;
  REQUIRE_EQ(rnf_input_set_layout_json(s5b::layout().toJson().c_str()), RN_OK);
  std::unique_ptr<rnf_menu, decltype(&rnf_menu_free)> menu(rnf_menu_new(RNF_MENU_FEATURE_LAYOUT_TURBO | RNF_MENU_FEATURE_OSK), rnf_menu_free);
  const int page = rnf_menu_page_find(menu.get(), "controls.turbo_page");
  REQUIRE(page >= 0);
  rnf_menu_page_info rootInfo{};
  REQUIRE(rnf_menu_page_get(menu.get(), page, &rootInfo));
  CHECK(rootInfo.count <= RNF_MENU_MAX_ITEMS);
  CHECK(rnf_menu_item_find(menu.get(), page, "controls.turbo") >= 0);
  CHECK(rnf_menu_item_find(menu.get(), page, "controls.turbo_duty") >= 0);
  for (size_t i = 0; i < s5b::layout().buttons.size(); ++i) {
    const std::string prefix = "controls.button_turbo." + std::to_string(i);
    const int buttonPage = rnf_menu_page_find(menu.get(), prefix.c_str());
    REQUIRE(buttonPage >= 0);
    rnf_menu_page_info buttonInfo{};
    REQUIRE(rnf_menu_page_get(menu.get(), buttonPage, &buttonInfo));
    CHECK_EQ(buttonInfo.count, size_t(3));
    int mode = rnf_menu_item_find(menu.get(), buttonPage, (prefix + ".mode").c_str());
    REQUIRE(mode >= 0);
    CHECK_EQ(std::string(rnf_menu_item_choice(menu.get(), buttonPage, mode, 0)), "Global");
    CHECK_EQ(std::string(rnf_menu_item_choice(menu.get(), buttonPage, mode, 1)), "Custom");
    CHECK(rnf_menu_item_find(menu.get(), buttonPage, (prefix + ".period").c_str()) >= 0);
    CHECK(rnf_menu_item_find(menu.get(), buttonPage, (prefix + ".duty").c_str()) >= 0);
  }
  if (!RN_PRODUCT_NES) {
    for (size_t p = 0; p < rnf_menu_page_count(menu.get()); ++p) {
      rnf_menu_item_info item{};
      for (size_t i = 0; rnf_menu_item_get(menu.get(), p, i, &item); ++i) {
        CHECK(std::string(item.id).find("test_cartridge") == std::string::npos);
        CHECK(std::string(item.label).find("Test Cartridge") == std::string::npos);
      }
    }
  }
}

TEST_CASE("S5b layout Turbo pages are opt-in and legacy Controls retain their exact rows") {
  LayoutScope scope;
  const auto ids = [](rnf_menu* menu, const char* pageId) {
    const int page = rnf_menu_page_find(menu, pageId);
    REQUIRE(page >= 0);
    std::vector<std::string> result;
    rnf_menu_item_info item{};
    for (size_t i = 0; rnf_menu_item_get(menu, size_t(page), i, &item); ++i) result.emplace_back(item.id);
    return result;
  };
  const std::vector<std::string> controls = {"controls.controller", "controls.keyboard", "controls.confirm",
      "controls.dpad_paused", "controls.osk", "controls.detail"};
  const std::vector<std::string> detail = {"controls.rewind_pause", "controls.turbo", "controls.turbo_duty",
      "controls.socd", "controls.stick", "controls.reset"};
  for (const bool generic : {false, true}) {
    REQUIRE_EQ(rnf_input_set_layout_json(generic ? s5b::layout().toJson().c_str() : nullptr), RN_OK);
    std::unique_ptr<rnf_menu, decltype(&rnf_menu_free)> legacy(rnf_menu_new(RNF_MENU_FEATURE_OSK), rnf_menu_free);
    CHECK(ids(legacy.get(), "settings.controls") == controls);
    CHECK(ids(legacy.get(), "controls.detail") == detail);
    CHECK_EQ(rnf_menu_page_find(legacy.get(), "controls.turbo_page"), -1);
    CHECK_EQ(rnf_menu_page_find(legacy.get(), "controls.turbo_buttons"), -1);
    for (size_t i = 0; i < rnf_input_button_count(); ++i)
      CHECK_EQ(rnf_menu_page_find(legacy.get(), ("controls.button_turbo." + std::to_string(i)).c_str()), -1);
  }
  std::unique_ptr<rnf_menu, decltype(&rnf_menu_free)> enabled(
      rnf_menu_new(RNF_MENU_FEATURE_OSK | RNF_MENU_FEATURE_LAYOUT_TURBO), rnf_menu_free);
  const auto enabledControls = ids(enabled.get(), "settings.controls");
  const auto enabledDetail = ids(enabled.get(), "controls.detail");
  CHECK(std::find(enabledControls.begin(), enabledControls.end(), "controls.osk") == enabledControls.end());
  CHECK(std::find(enabledDetail.begin(), enabledDetail.end(), "controls.osk") != enabledDetail.end());
  CHECK(std::find(enabledDetail.begin(), enabledDetail.end(), "controls.turbo") == enabledDetail.end());
  CHECK(ids(enabled.get(), "controls.turbo_page") == std::vector<std::string>({"controls.turbo", "controls.turbo_duty", "controls.turbo_buttons"}));
  CHECK(enabledControls.size() <= RNF_MENU_MAX_ITEMS);
  CHECK(enabledDetail.size() <= RNF_MENU_MAX_ITEMS);
}

TEST_CASE("P stick defaults use arcade rows turbo and dedicated hotkeys") {
  LayoutScope scope;
  for (int count : {1, 2, 3, 6}) {
    rn::InputLayout layout;
    layout.name = "fbneo:profile-test";
    layout.recordBytes = 5;
    for (int player = 1; player <= 2; ++player) {
      const auto prefix = "p" + std::to_string(player) + ".";
      for (int n = 1; n <= count; ++n)
        layout.buttons.push_back({prefix + "b" + std::to_string(n), "", uint16_t(player - 1), uint8_t(n - 1)});
      layout.buttons.push_back({prefix + "coin", "", uint16_t(player + 2), 0});
      layout.buttons.push_back({prefix + "start", "", uint16_t(player + 2), 1});
      layout.buttons.push_back({prefix + "up", "", uint16_t(player + 2), 2});
    }
    REQUIRE_EQ(rnf_input_set_layout_json(layout.toJson().c_str()), RN_OK);
    CHECK_EQ(take(rnf_input_bindings_filename_for_kind(0)), "bindings-fbneo%3Aprofile-test.json");
    CHECK_EQ(take(rnf_input_bindings_filename_for_kind(1)), "bindings-fbneo%3Aprofile-test@stick.json");
    rnf_input_config* config = nullptr;
    REQUIRE_EQ(rnf_input_config_parse(take(rnf_input_default_config_json_for_kind(RNF_KEYBOARD_SDL, 1)).c_str(), &config), RN_OK);
    std::set<std::string> pairs;
    for (size_t i = 0; i < rnf_input_config_binding_count(config); ++i) {
      rnf_binding b{};
      REQUIRE(rnf_input_config_binding_get(config, i, &b));
      pairs.insert(std::string(b.input) + "=" + b.action);
    }
    rnf_input_config_free(config);
    const char* elements[] = {"face.west", "face.north", "rightShoulder", "face.south", "face.east", "rightTrigger"};
    for (int slot = 0; slot < 2; ++slot) {
      const auto gc = "gc" + std::to_string(slot) + ":";
      const auto player = "p" + std::to_string(slot + 1) + ".";
      for (int n = 0; n < count; ++n) CHECK(pairs.count(gc + elements[n] + "=" + player + "b" + std::to_string(n + 1)));
      if (count <= 3) for (int n = 0; n < count; ++n)
        CHECK(pairs.count(gc + elements[n + 3] + "=" + player + "b" + std::to_string(n + 1) + ".turbo"));
      CHECK(pairs.count(gc + "options=" + player + "coin"));
      CHECK(pairs.count(gc + "menu=" + player + "start"));
      CHECK(pairs.count(gc + "dpad.up=" + player + "up"));
      CHECK(pairs.count(gc + "lstick.up=" + player + "up"));
    }
    CHECK(pairs.count("gc0:leftTrigger=hk.rewind"));
    CHECK(pairs.count("gc0:leftShoulder=hk.fast_forward"));
    CHECK(pairs.count("gc0:leftShoulder+gc0:leftTrigger=hk.menu") || pairs.count("gc0:leftTrigger+gc0:leftShoulder=hk.menu"));
    CHECK_FALSE(pairs.count("gc0:leftShoulder+gc0:rightShoulder=hk.menu"));
    CHECK_FALSE(pairs.count("gc0:rightTrigger=hk.fast_forward"));
    CHECK_EQ(take(rnf_input_default_config_json_for_kind(RNF_KEYBOARD_SDL, 0)), take(rnf_input_default_config_json(RNF_KEYBOARD_SDL)));
  }
}

TEST_CASE("P NES ignores device kind for filenames and complete defaults") {
  LayoutScope scope;
  REQUIRE_EQ(rnf_input_set_layout_json(nullptr), RN_OK);
  for (auto scheme : {RNF_KEYBOARD_SDL, RNF_KEYBOARD_MACOS}) {
    CHECK_EQ(take(rnf_input_default_config_json_for_kind(scheme, 1)), take(rnf_input_default_config_json(scheme)));
    CHECK_EQ(take(rnf_input_default_config_json_for_kind(scheme, 0)), take(rnf_input_default_config_json(scheme)));
  }
  CHECK_EQ(take(rnf_input_bindings_filename_for_kind(1)), "bindings.json");
  CHECK_EQ(take(rnf_input_bindings_filename_for_kind(0)), "bindings.json");
}

TEST_CASE("D stick diagram navigation reaches every assignable element and the kind row") {
  struct Point { float x, y; };
  const auto family = rnf_diagram_family_for_layout(RNF_FAMILY_XBOX, 1, 1);
  // Match the UI: HOME belongs to the system, so it is drawn but not focusable.
  for (float scale : {0.5f, 1.0f, 2.0f}) {
    std::vector<Point> centers;
    for (size_t i = 0; i < rnf_diagram_element_count(family); ++i) {
      rnf_diagram_element e{};
      REQUIRE(rnf_diagram_element_get(family, i, &e));
      if (!rnf_input_element_ignored(e.element))
        centers.push_back({float(e.cx) * scale, float(e.cy) * scale});
    }
    REQUIRE_EQ(centers.size(), size_t(14));  // four directions, eight buttons, Select + Start
    centers.push_back({float(RNF_DIAGRAM_CANVAS_WIDTH / 2) * scale, -30 * scale});
    // Strong connectivity: changing kind or selecting any element must never strand focus.
    for (size_t start = 0; start < centers.size(); ++start) {
      std::set<int> visited{int(start)};
      std::vector<int> pending{int(start)};
      while (!pending.empty()) {
        const int current = pending.back();
        pending.pop_back();
        for (auto dir : {Point{-1, 0}, Point{1, 0}, Point{0, -1}, Point{0, 1}}) {
          int next = rnl::diagramFocusNext(centers, current, int(dir.x), int(dir.y));
          REQUIRE(next >= 0);
          REQUIRE(next < int(centers.size()));
          if (visited.insert(next).second) pending.push_back(next);
        }
      }
      CHECK_EQ(visited.size(), centers.size());
    }
  }
}

TEST_CASE("D pad diagram navigation reaches every assignable element (every family, NES and arcade)") {
  struct Point { float x, y; };
  for (auto fam : {RNF_FAMILY_NINTENDO, RNF_FAMILY_XBOX, RNF_FAMILY_PLAYSTATION, RNF_FAMILY_GENERIC, RNF_FAMILY_STEAM_DECK}) {
    for (int kinds : {0, 1}) {
      const auto family = rnf_diagram_family_for_layout(fam, kinds, 0);
      std::vector<Point> centers;
      std::vector<std::string> names, groups;
      for (size_t i = 0; i < rnf_diagram_element_count(family); ++i) {
        rnf_diagram_element e{};
        REQUIRE(rnf_diagram_element_get(family, i, &e));
        if (rnf_input_element_ignored(e.element)) continue;
        centers.push_back({float(e.cx), float(e.cy)});
        names.push_back(e.element);
        // As ui_settings.cpp: a stick's arrows and its click share the click as their hub.
        const std::string el = e.element;
        groups.push_back(e.kind == RNF_DIAGRAM_STICK_CLICK ? (el == "leftThumb" ? "lstick" : "rstick")
                         : e.kind == RNF_DIAGRAM_STICK_DIRECTION && e.group ? e.group : "");
      }
      if (kinds) centers.push_back({float(RNF_DIAGRAM_CANVAS_WIDTH / 2), -30}), names.push_back("__kind__"), groups.push_back("");
      std::vector<int> hubs(centers.size(), -1);
      for (size_t i = 0; i < centers.size(); ++i)
        for (size_t j = 0; j < centers.size(); ++j)
          if ((groups[i] == "lstick" && names[j] == "leftThumb") || (groups[i] == "rstick" && names[j] == "rightThumb")) hubs[i] = int(j);
      // L3 / R3: entered straight from outside their stick (not only from its own arrows).
      for (const char* click : {"leftThumb", "rightThumb"}) {
        const auto at = std::find(names.begin(), names.end(), click);
        REQUIRE(at != names.end());
        const int target = int(at - names.begin());
        bool fromOutside = false;
        for (size_t i = 0; i < centers.size(); ++i)
          for (auto dir : {Point{-1, 0}, Point{1, 0}, Point{0, -1}, Point{0, 1}})
            if (hubs[i] != target && rnl::diagramFocusNext(centers, int(i), int(dir.x), int(dir.y), &hubs) == target) fromOutside = true;
        if (!fromOutside) MESSAGE("family " << int(fam) << " kinds " << kinds << ": " << click << " only from its own arrows");
        CHECK(fromOutside);
      }
      for (size_t start = 0; start < centers.size(); ++start) {
        std::set<int> visited{int(start)};
        std::vector<int> pending{int(start)};
        while (!pending.empty()) {
          const int current = pending.back();
          pending.pop_back();
          for (auto dir : {Point{-1, 0}, Point{1, 0}, Point{0, -1}, Point{0, 1}}) {
            int next = rnl::diagramFocusNext(centers, current, int(dir.x), int(dir.y), &hubs);
            if (visited.insert(next).second) pending.push_back(next);
          }
        }
        std::string missing;
        for (size_t i = 0; i < centers.size(); ++i)
          if (!visited.count(int(i))) missing += " " + names[i];
        if (!missing.empty())
          MESSAGE("family " << int(fam) << " kinds " << kinds << " from " << names[start] << ": unreachable" << missing);
        CHECK(missing.empty());
      }
    }
  }
}
