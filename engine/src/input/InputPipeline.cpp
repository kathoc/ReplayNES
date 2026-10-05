#include "input/InputPipeline.h"

#include "util/Json.h"

namespace rn {

namespace {
const char* kBtn[8] = {"a", "b", "select", "start", "up", "down", "left", "right"};
const char* kHk[HK_COUNT] = {"pause", "frame_advance", "rewind", "slow", "bookmark", "soft_reset",
                             "power_cycle", "toggle_mode", "save", "undo_take", "fast_forward", "step_back"};
enum : uint8_t { B_UP = 0x10, B_DOWN = 0x20, B_LEFT = 0x40, B_RIGHT = 0x80 };
}  // namespace

bool actionFromName(const std::string& n, int& a) {
  for (int p = 0; p < 2; ++p) {
    std::string pre = p == 0 ? "p1." : "p2.";
    for (int b = 0; b < 8; ++b)
      if (n == pre + kBtn[b]) { a = p * 8 + b; return true; }
    if (n == pre + "turbo_a") { a = p == 0 ? kActP1TurboA : kActP2TurboA; return true; }
    if (n == pre + "turbo_b") { a = p == 0 ? kActP1TurboB : kActP2TurboB; return true; }
  }
  for (int h = 0; h < HK_COUNT; ++h)
    if (n == std::string("hk.") + kHk[h]) { a = kActHotkeyBase + h; return true; }
  return false;
}

std::string actionName(int a) {
  if (a >= 0 && a < 16) return std::string(a < 8 ? "p1." : "p2.") + kBtn[a % 8];
  switch (a) {
    case kActP1TurboA: return "p1.turbo_a";
    case kActP1TurboB: return "p1.turbo_b";
    case kActP2TurboA: return "p2.turbo_a";
    case kActP2TurboB: return "p2.turbo_b";
  }
  if (a >= kActHotkeyBase && a < kActHotkeyBase + HK_COUNT) return std::string("hk.") + kHk[a - kActHotkeyBase];
  return "";
}

Status InputPipeline::bind(const std::string& physical, const std::string& action) {
  int a;
  if (physical.empty()) return Error(Err::InvalidArg, "empty physical input id");
  if (!actionFromName(action, a)) return Error(Err::InvalidArg, "unknown action '" + action + "'");
  std::lock_guard<std::mutex> g(m_);
  auto range = bind_.equal_range(physical);
  for (auto it = range.first; it != range.second; ++it)
    if (it->second == a) return Status::Ok();
  bind_.emplace(physical, a);
  return Status::Ok();
}

Status InputPipeline::unbind(const std::string& physical, const std::string& action) {
  std::lock_guard<std::mutex> g(m_);
  if (action.empty()) { bind_.erase(physical); return Status::Ok(); }
  int a;
  if (!actionFromName(action, a)) return Error(Err::InvalidArg, "unknown action '" + action + "'");
  auto range = bind_.equal_range(physical);
  for (auto it = range.first; it != range.second; ++it)
    if (it->second == a) { bind_.erase(it); return Status::Ok(); }
  return Error(Err::NotFound, "binding not found");
}

void InputPipeline::clearBindings() {
  std::lock_guard<std::mutex> g(m_);
  bind_.clear();
}

std::vector<std::pair<std::string, std::string>> InputPipeline::bindings() const {
  std::lock_guard<std::mutex> g(m_);
  std::vector<std::pair<std::string, std::string>> v;
  for (auto& kv : bind_) v.emplace_back(kv.first, actionName(kv.second));
  return v;
}

Status InputPipeline::setTurbo(uint32_t period, uint32_t duty) {
  if (period < 1 || duty < 1 || duty > period) return Error(Err::InvalidArg, "turbo needs 1 <= duty <= period");
  std::lock_guard<std::mutex> g(m_);
  period_ = period;
  duty_ = duty;
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
      int a = b->second;
      if (a >= kActHotkeyBase) hkEdges_ |= 1u << (a - kActHotkeyBase);
      else if (a < 32) { latch_ |= uint64_t(1) << a; latchSeq_[a] = s; }
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
}

void InputPipeline::releasePrefix(const std::string& prefix) {
  std::lock_guard<std::mutex> g(m_);
  for (auto it = pressed_.begin(); it != pressed_.end();)
    it = it->first.compare(0, prefix.size(), prefix) == 0 ? pressed_.erase(it) : std::next(it);
}

void InputPipeline::releaseAll() {
  std::lock_guard<std::mutex> g(m_);
  pressed_.clear();
  latch_ = 0;
  hkEdges_ = 0;
  turboStart_.clear();
}

bool InputPipeline::actionHeld(int action, uint64_t* lastSeq) const {
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

void InputPipeline::sampleGame(uint64_t frame, uint8_t& p1, uint8_t& p2) {
  std::lock_guard<std::mutex> g(m_);
  uint8_t out[2] = {0, 0};
  uint64_t seqOf[16] = {};
  for (int a = 0; a < 16; ++a) {
    uint64_t s = 0;
    bool held = actionHeld(a, &s);
    bool latched = (latch_ >> a) & 1;
    if (held || latched) {
      out[a / 8] |= uint8_t(1u << (a % 8));
      seqOf[a] = held ? s : latchSeq_[a];
    }
  }
  const int turbo[4] = {kActP1TurboA, kActP1TurboB, kActP2TurboA, kActP2TurboB};
  for (int i = 0; i < 4; ++i) {
    int a = turbo[i];
    bool on = actionHeld(a, nullptr) || ((latch_ >> a) & 1);
    if (!on) { turboStart_.erase(a); continue; }
    auto it = turboStart_.find(a);
    if (it == turboStart_.end() || frame < it->second) it = turboStart_.insert_or_assign(a, frame).first;
    uint64_t phase = (frame - it->second) % period_;
    if (phase < duty_) out[i / 2] |= uint8_t(i % 2 == 0 ? 0x01 : 0x02);
  }
  for (int p = 0; p < 2; ++p) {
    auto resolve = [&](uint8_t m1, uint8_t m2, int a1, int a2) {
      if ((out[p] & (m1 | m2)) != (m1 | m2)) return;
      switch (socd_) {
        case Socd::Neutral: out[p] &= uint8_t(~(m1 | m2)); break;
        case Socd::LastWins:
          out[p] &= uint8_t(~(seqOf[p * 8 + a1] >= seqOf[p * 8 + a2] ? m2 : m1));
          break;
        case Socd::Allow: break;
      }
    };
    resolve(B_UP, B_DOWN, 4, 5);
    resolve(B_LEFT, B_RIGHT, 6, 7);
  }
  latch_ = 0;
  p1 = out[0];
  p2 = out[1];
}

void InputPipeline::pollHotkeys(uint32_t& pressedEdges, uint32_t& held) {
  std::lock_guard<std::mutex> g(m_);
  pressedEdges = hkEdges_;
  hkEdges_ = 0;
  held = 0;
  for (int h = 0; h < HK_COUNT; ++h)
    if (actionHeld(kActHotkeyBase + h, nullptr)) held |= 1u << h;
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
    e.set("action", actionName(kv.second));
    b.push(e);
  }
  j.set("bindings", b);
  Json t = Json::object();
  t.set("period", period_);
  t.set("duty", duty_);
  j.set("turbo", t);
  j.set("socd", socdName(socd_));
  j.set("analogThreshold", double(threshold_));
  return j.dump();
}

Status InputPipeline::fromJson(const std::string& text) {
  Json j;
  std::string err;
  if (!Json::parse(text, j, &err) || !j.isObject()) return Error(Err::InvalidArg, "invalid input config JSON: " + err);
  if (j["version"].asInt() != 1) return Error(Err::UnsupportedFormat, "unsupported input config version");
  // Validate everything before mutating.
  std::multimap<std::string, int> nb;
  for (auto& e : j["bindings"].items()) {
    int a;
    if (!actionFromName(e["action"].asString(), a) || e["input"].asString().empty())
      return Error(Err::InvalidArg, "bad binding entry");
    nb.emplace(e["input"].asString(), a);
  }
  int64_t period = j["turbo"]["period"].asInt(2), duty = j["turbo"]["duty"].asInt(1);
  if (period < 1 || duty < 1 || duty > period) return Error(Err::InvalidArg, "bad turbo settings");
  std::string s = j["socd"].asString();
  Socd so = s == "last_wins" ? Socd::LastWins : s == "allow" ? Socd::Allow : Socd::Neutral;
  if (!s.empty() && s != "last_wins" && s != "allow" && s != "neutral") return Error(Err::InvalidArg, "bad socd policy");
  double th = j["analogThreshold"].asDouble(0.5);
  if (!(th > 0 && th < 1)) return Error(Err::InvalidArg, "bad analog threshold");
  std::lock_guard<std::mutex> g(m_);
  bind_ = std::move(nb);
  period_ = uint32_t(period);
  duty_ = uint32_t(duty);
  socd_ = so;
  threshold_ = float(th);
  return Status::Ok();
}

}  // namespace rn
