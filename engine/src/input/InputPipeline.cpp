#include "input/InputPipeline.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "util/Json.h"

namespace rn {

namespace {
const char* kHk[HK_COUNT] = {"pause", "frame_advance", "rewind", "slow", "bookmark", "soft_reset",
                             "power_cycle", "toggle_mode", "save", "undo_take", "fast_forward", "step_back", "menu"};

int hotkeyIndex(const std::string& action) {
  for (int h = 0; h < HK_COUNT; ++h)
    if (action == std::string("hk.") + kHk[h]) return h;
  return -1;
}

void setBit(InputRecord& r, const InputLayout::Button& b) { r.setByte(b.byte, uint8_t(r.byte(b.byte) | (1u << b.bit))); }
void clearBit(InputRecord& r, const InputLayout::Button& b) { r.setByte(b.byte, uint8_t(r.byte(b.byte) & ~(1u << b.bit))); }
bool hasBit(const InputRecord& r, const InputLayout::Button& b) { return (r.byte(b.byte) >> b.bit) & 1; }
}  // namespace

bool InputPipeline::knownAction(const std::string& a) const {
  if (hotkeyIndex(a) >= 0) return true;
  for (const auto& b : layout_.buttons)
    if (a == b.id || a == layout_.turboOf(b)) return true;
  for (const auto& x : layout_.axes)
    if (a == x.id) return true;
  return false;
}

Status InputPipeline::setLayout(const InputLayout& layout) {
  RN_TRY(layout.validate());
  std::lock_guard<std::mutex> g(m_);
  layout_ = layout;
  for (auto it = bind_.begin(); it != bind_.end();) it = knownAction(it->second) ? std::next(it) : bind_.erase(it);
  for (auto it = buttonTurbo_.begin(); it != buttonTurbo_.end();) it = knownAction(it->first) ? std::next(it) : buttonTurbo_.erase(it);
  latched_.clear();
  turboStart_.clear();
  return Status::Ok();
}

InputLayout InputPipeline::layout() const {
  std::lock_guard<std::mutex> g(m_);
  return layout_;
}

Status InputPipeline::bind(const std::string& physical, const std::string& action) {
  if (physical.empty()) return Error(Err::InvalidArg, "empty physical input id");
  std::lock_guard<std::mutex> g(m_);
  if (!knownAction(action)) return Error(Err::InvalidArg, "unknown action '" + action + "'");
  auto range = bind_.equal_range(physical);
  for (auto it = range.first; it != range.second; ++it)
    if (it->second == action) return Status::Ok();
  bind_.emplace(physical, action);
  return Status::Ok();
}

Status InputPipeline::unbind(const std::string& physical, const std::string& action) {
  std::lock_guard<std::mutex> g(m_);
  if (action.empty()) { bind_.erase(physical); return Status::Ok(); }
  if (!knownAction(action)) return Error(Err::InvalidArg, "unknown action '" + action + "'");
  auto range = bind_.equal_range(physical);
  for (auto it = range.first; it != range.second; ++it)
    if (it->second == action) { bind_.erase(it); return Status::Ok(); }
  return Error(Err::NotFound, "binding not found");
}

void InputPipeline::clearBindings() {
  std::lock_guard<std::mutex> g(m_);
  bind_.clear();
}

std::vector<std::pair<std::string, std::string>> InputPipeline::bindings() const {
  std::lock_guard<std::mutex> g(m_);
  return std::vector<std::pair<std::string, std::string>>(bind_.begin(), bind_.end());
}

Status InputPipeline::setTurbo(uint32_t period, uint32_t duty) {
  if (period < 1 || duty < 1 || duty > period) return Error(Err::InvalidArg, "turbo needs 1 <= duty <= period");
  std::lock_guard<std::mutex> g(m_);
  period_ = period;
  duty_ = duty;
  return Status::Ok();
}

Status InputPipeline::setButtonTurbo(const std::string& button, uint32_t period, uint32_t duty) {
  std::lock_guard<std::mutex> g(m_);
  bool known = false;
  for (const auto& b : layout_.buttons) known = known || b.id == button;
  if (!known) return Error(Err::InvalidArg, "unknown button '" + button + "'");
  if (period == 0) { buttonTurbo_.erase(button); return Status::Ok(); }
  if (duty < 1 || duty > period) return Error(Err::InvalidArg, "turbo needs 1 <= duty <= period");
  buttonTurbo_[button] = Timing{period, duty};
  return Status::Ok();
}

void InputPipeline::setSocd(Socd s) {
  std::lock_guard<std::mutex> g(m_);
  socd_ = s;
}

Status InputPipeline::setAnalogThreshold(float t) {
  if (!(t > 0.f && t < 1.f)) return Error(Err::InvalidArg, "threshold must be in (0,1)");
  std::lock_guard<std::mutex> g(m_);
  threshold_ = t;
  return Status::Ok();
}

void InputPipeline::setPressedLocked(const std::string& physical, bool pressed) {
  auto it = pressed_.find(physical);
  if (pressed) {
    if (it != pressed_.end()) return;  // key repeat / unchanged
    uint64_t s = ++seq_;
    pressed_[physical] = s;
    auto range = bind_.equal_range(physical);
    for (auto b = range.first; b != range.second; ++b) {
      int h = hotkeyIndex(b->second);
      if (h >= 0) hkEdges_ |= 1u << h;
      else latched_[b->second] = s;
    }
  } else if (it != pressed_.end()) {
    pressed_.erase(it);
  }
}

void InputPipeline::setPressed(const std::string& physical, bool pressed) {
  std::lock_guard<std::mutex> g(m_);
  setPressedLocked(physical, pressed);
}

void InputPipeline::setAxis(const std::string& stick, float x, float y) {
  std::lock_guard<std::mutex> g(m_);
  setPressedLocked(stick + ".left", x <= -threshold_);
  setPressedLocked(stick + ".right", x >= threshold_);
  setPressedLocked(stick + ".up", y >= threshold_);
  setPressedLocked(stick + ".down", y <= -threshold_);
  analog_[stick + ".x"] = x;
  analog_[stick + ".y"] = y;
  analog_[stick + ".-x"] = -x;
  analog_[stick + ".-y"] = -y;
}

void InputPipeline::setAxisValue(const std::string& source, float value) {
  std::lock_guard<std::mutex> g(m_);
  analog_[source] = value;
}

void InputPipeline::releasePrefix(const std::string& prefix) {
  std::lock_guard<std::mutex> g(m_);
  for (auto it = pressed_.begin(); it != pressed_.end();)
    it = it->first.compare(0, prefix.size(), prefix) == 0 ? pressed_.erase(it) : std::next(it);
  for (auto it = analog_.begin(); it != analog_.end();)
    it = it->first.compare(0, prefix.size(), prefix) == 0 ? analog_.erase(it) : std::next(it);
}

void InputPipeline::releaseAll() {
  std::lock_guard<std::mutex> g(m_);
  pressed_.clear();
  analog_.clear();
  latched_.clear();
  hkEdges_ = 0;
  turboStart_.clear();
}

bool InputPipeline::actionHeld(const std::string& action, uint64_t* lastSeq) const {
  bool held = false;
  uint64_t best = 0;
  for (auto& kv : pressed_) {
    auto range = bind_.equal_range(kv.first);
    for (auto b = range.first; b != range.second; ++b)
      if (b->second == action) { held = true; if (kv.second > best) best = kv.second; }
  }
  if (lastSeq) *lastSeq = best;
  return held;
}

// Sum of the analog sources bound to the axis action (a digital input bound to it counts 1).
float InputPipeline::axisValue(const std::string& action) const {
  float v = 0;
  for (auto& kv : bind_) {
    if (kv.second != action) continue;
    auto a = analog_.find(kv.first);
    if (a != analog_.end()) v += a->second;
    else if (pressed_.count(kv.first)) v += 1.f;
  }
  return std::max(-1.f, std::min(1.f, v));
}

void InputPipeline::sampleRecord(uint64_t frame, InputRecord& out) {
  std::lock_guard<std::mutex> g(m_);
  out = InputRecord{};
  std::map<std::string, uint64_t> seqOf;
  for (const auto& b : layout_.buttons) {
    uint64_t s = 0;
    bool held = actionHeld(b.id, &s);
    auto l = latched_.find(b.id);
    if (held || l != latched_.end()) {
      setBit(out, b);
      seqOf[b.id] = held ? s : l->second;
    }
  }
  for (const auto& b : layout_.buttons) {
    const std::string t = layout_.turboOf(b);
    bool on = actionHeld(t, nullptr) || latched_.count(t);
    if (!on) { turboStart_.erase(t); continue; }
    auto it = turboStart_.find(t);
    if (it == turboStart_.end() || frame < it->second) it = turboStart_.insert_or_assign(t, frame).first;
    auto own = buttonTurbo_.find(b.id);
    const uint32_t period = own != buttonTurbo_.end() ? own->second.period : period_;
    const uint32_t duty = own != buttonTurbo_.end() ? own->second.duty : duty_;
    if ((frame - it->second) % period < duty) setBit(out, b);
  }
  for (const auto& pr : layout_.opposite) {
    const InputLayout::Button *x = nullptr, *y = nullptr;
    for (const auto& b : layout_.buttons) {
      if (b.id == pr.first) x = &b;
      if (b.id == pr.second) y = &b;
    }
    if (!x || !y || !hasBit(out, *x) || !hasBit(out, *y)) continue;
    switch (socd_) {
      case Socd::Neutral: clearBit(out, *x); clearBit(out, *y); break;
      case Socd::LastWins: clearBit(out, seqOf[x->id] >= seqOf[y->id] ? *y : *x); break;
      case Socd::Allow: break;
    }
  }
  for (const auto& a : layout_.axes) {
    int16_t v = int16_t(std::lround(axisValue(a.id) * 32767.f));
    out.setByte(a.byte, uint8_t(uint16_t(v) & 0xFF));
    out.setByte(a.byte + 1u, uint8_t(uint16_t(v) >> 8));
  }
  latched_.clear();
}

void InputPipeline::sampleGame(uint64_t frame, uint8_t& p1, uint8_t& p2) {
  InputRecord r;
  sampleRecord(frame, r);
  p1 = r.p1;
  p2 = r.p2;
}

void InputPipeline::pollHotkeys(uint32_t& pressedEdges, uint32_t& held) {
  std::lock_guard<std::mutex> g(m_);
  pressedEdges = hkEdges_;
  hkEdges_ = 0;
  held = 0;
  for (int h = 0; h < HK_COUNT; ++h)
    if (actionHeld(std::string("hk.") + kHk[h], nullptr)) held |= 1u << h;
}

static const char* socdName(Socd s) {
  return s == Socd::LastWins ? "last_wins" : s == Socd::Allow ? "allow" : "neutral";
}

std::string InputPipeline::toJson() const {
  std::lock_guard<std::mutex> g(m_);
  Json j = Json::object();
  j.set("version", 1);
  Json b = Json::array();
  for (auto& kv : bind_) {
    Json e = Json::object();
    e.set("input", kv.first);
    e.set("action", kv.second);
    b.push(e);
  }
  j.set("bindings", b);
  Json t = Json::object();
  t.set("period", period_);
  t.set("duty", duty_);
  j.set("turbo", t);
  if (!buttonTurbo_.empty()) {  // only when set, so configurations without it read back unchanged
    Json bt = Json::array();
    for (auto& kv : buttonTurbo_) {
      Json e = Json::object();
      e.set("button", kv.first);
      e.set("period", kv.second.period);
      e.set("duty", kv.second.duty);
      bt.push(e);
    }
    j.set("buttonTurbo", bt);
  }
  j.set("socd", socdName(socd_));
  j.set("analogThreshold", double(threshold_));
  return j.dump();
}

Status InputPipeline::fromJson(const std::string& text) {
  Json j;
  std::string err;
  if (!Json::parse(text, j, &err) || !j.isObject()) return Error(Err::InvalidArg, "invalid input config JSON: " + err);
  if (j["version"].asInt() != 1) return Error(Err::UnsupportedFormat, "unsupported input config version");
  std::lock_guard<std::mutex> g(m_);
  // Validate everything before mutating.
  std::multimap<std::string, std::string> nb;
  for (auto& e : j["bindings"].items()) {
    if (!knownAction(e["action"].asString()) || e["input"].asString().empty())
      return Error(Err::InvalidArg, "bad binding entry");
    nb.emplace(e["input"].asString(), e["action"].asString());
  }
  int64_t period = j["turbo"]["period"].asInt(2), duty = j["turbo"]["duty"].asInt(1);
  if (period < 1 || duty < 1 || duty > period) return Error(Err::InvalidArg, "bad turbo settings");
  std::map<std::string, Timing> bt;
  for (auto& e : j["buttonTurbo"].items()) {
    const std::string id = e["button"].asString();
    int64_t p = e["period"].asInt(0), d = e["duty"].asInt(0);
    bool known = false;
    for (const auto& b : layout_.buttons) known = known || b.id == id;
    if (!known || p < 1 || d < 1 || d > p) return Error(Err::InvalidArg, "bad button turbo entry");
    bt[id] = Timing{uint32_t(p), uint32_t(d)};
  }
  std::string s = j["socd"].asString();
  Socd so = s == "last_wins" ? Socd::LastWins : s == "allow" ? Socd::Allow : Socd::Neutral;
  if (!s.empty() && s != "last_wins" && s != "allow" && s != "neutral") return Error(Err::InvalidArg, "bad socd policy");
  double th = j["analogThreshold"].asDouble(0.5);
  if (!(th > 0 && th < 1)) return Error(Err::InvalidArg, "bad analog threshold");
  bind_ = std::move(nb);
  period_ = uint32_t(period);
  duty_ = uint32_t(duty);
  buttonTurbo_ = std::move(bt);
  socd_ = so;
  threshold_ = float(th);
  return Status::Ok();
}

}  // namespace rn
