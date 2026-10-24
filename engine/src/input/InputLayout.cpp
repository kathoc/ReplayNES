#include "input/InputLayout.h"

#include <set>

#include "util/Json.h"

namespace rn {

const InputLayout& InputLayout::nes() {
  static const InputLayout layout = [] {
    InputLayout l;
    l.name = "nes";
    l.recordBytes = 3;
    const char* names[8] = {"a", "b", "select", "start", "up", "down", "left", "right"};
    for (int port = 0; port < 2; ++port) {
      std::string pre = port == 0 ? "p1." : "p2.";
      for (int bit = 0; bit < 8; ++bit) {
        Button b;
        b.id = pre + names[bit];
        b.byte = uint16_t(port);
        b.bit = uint8_t(bit);
        // The NES turbo actions keep their names, so saved bindings read and write unchanged.
        if (bit == 0) b.turboId = pre + "turbo_a";
        if (bit == 1) b.turboId = pre + "turbo_b";
        l.buttons.push_back(b);
      }
      l.opposite.push_back({pre + "up", pre + "down"});
      l.opposite.push_back({pre + "left", pre + "right"});
    }
    return l;
  }();
  return layout;
}

Status InputLayout::validate() const {
  if (name.empty()) return Error(Err::InvalidArg, "input layout without a name");
  if (recordBytes < 3 || recordBytes > kInputRecordBytes)
    return Error(Err::InvalidArg, "input layout record size must be 3.." + std::to_string(kInputRecordBytes));
  std::set<std::string> ids;
  std::set<std::pair<uint16_t, uint8_t>> bits;
  std::set<uint16_t> axisBytes;
  for (const Button& b : buttons) {
    if (b.id.empty() || !ids.insert(b.id).second || !ids.insert(turboOf(b)).second)
      return Error(Err::InvalidArg, "input layout: duplicate or empty action '" + b.id + "'");
    if (b.byte >= recordBytes || b.byte == kInputEventsByte || b.bit > 7 || !bits.insert({b.byte, b.bit}).second)
      return Error(Err::InvalidArg, "input layout: bad bit for '" + b.id + "'");
  }
  for (const Axis& a : axes) {
    if (a.id.empty() || !ids.insert(a.id).second) return Error(Err::InvalidArg, "input layout: duplicate axis");
    if (a.byte + 1u >= recordBytes || a.byte == kInputEventsByte || a.byte + 1u == kInputEventsByte)
      return Error(Err::InvalidArg, "input layout: bad axis bytes for '" + a.id + "'");
    for (int k = 0; k < 2; ++k) {
      for (int bit = 0; bit < 8; ++bit)
        if (bits.count({uint16_t(a.byte + k), uint8_t(bit)}))
          return Error(Err::InvalidArg, "input layout: axis '" + a.id + "' overlaps a button");
      if (!axisBytes.insert(uint16_t(a.byte + k)).second)
        return Error(Err::InvalidArg, "input layout: axes overlap at '" + a.id + "'");
    }
  }
  for (auto& pr : opposite) {
    auto known = [&](const std::string& id) {
      for (const Button& b : buttons)
        if (b.id == id) return true;
      return false;
    };
    if (!known(pr.first) || !known(pr.second)) return Error(Err::InvalidArg, "input layout: unknown opposite pair");
  }
  return Status::Ok();
}

std::string InputLayout::toJson() const {
  Json j = Json::object();
  j.set("name", name);
  j.set("recordBytes", recordBytes);
  Json bs = Json::array();
  for (const Button& b : buttons) {
    Json e = Json::object();
    e.set("id", b.id);
    if (!b.label.empty()) e.set("label", b.label);
    e.set("byte", uint32_t(b.byte));
    e.set("bit", uint32_t(b.bit));
    if (!b.turboId.empty()) e.set("turbo", b.turboId);
    bs.push(e);
  }
  j.set("buttons", bs);
  Json as = Json::array();
  for (const Axis& a : axes) {
    Json e = Json::object();
    e.set("id", a.id);
    if (!a.label.empty()) e.set("label", a.label);
    e.set("byte", uint32_t(a.byte));
    as.push(e);
  }
  j.set("axes", as);
  Json op = Json::array();
  for (auto& pr : opposite) {
    Json e = Json::array();
    e.push(pr.first);
    e.push(pr.second);
    op.push(e);
  }
  j.set("opposite", op);
  return j.dump(0);
}

Status InputLayout::fromJson(const std::string& text, InputLayout& out) {
  Json j;
  std::string err;
  if (!Json::parse(text, j, &err) || !j.isObject()) return Error(Err::InvalidArg, "invalid input layout JSON: " + err);
  InputLayout l;
  l.name = j["name"].asString();
  l.recordBytes = uint32_t(j["recordBytes"].asInt(0));
  for (const Json& e : j["buttons"].items()) {
    Button b;
    b.id = e["id"].asString();
    b.label = e["label"].asString();
    int64_t byte = e["byte"].asInt(-1), bit = e["bit"].asInt(-1);
    if (byte < 0 || byte > 0xFFFF || bit < 0 || bit > 7) return Error(Err::InvalidArg, "input layout: bad button");
    b.byte = uint16_t(byte);
    b.bit = uint8_t(bit);
    b.turboId = e["turbo"].asString();
    l.buttons.push_back(b);
  }
  for (const Json& e : j["axes"].items()) {
    Axis a;
    a.id = e["id"].asString();
    a.label = e["label"].asString();
    int64_t byte = e["byte"].asInt(-1);
    if (byte < 0 || byte > 0xFFFF) return Error(Err::InvalidArg, "input layout: bad axis");
    a.byte = uint16_t(byte);
    l.axes.push_back(a);
  }
  for (const Json& e : j["opposite"].items()) l.opposite.push_back({e.at(0).asString(), e.at(1).asString()});
  RN_TRY(l.validate());
  out = std::move(l);
  return Status::Ok();
}

}  // namespace rn
