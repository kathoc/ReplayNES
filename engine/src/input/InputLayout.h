// InputLayout: what one frame of game input holds for the loaded content, declared by the core.
// A frame is a byte string (InputRecord, Timeline.h): byte 0 and 1 are the NES pads, byte 2 the system
// events, further bytes whatever the layout says. Each button is one bit of one byte; each analog axis
// is a little-endian int16 at a byte offset. The NES layout is exactly the three bytes p1, p2, events,
// so NES takes, projects and bindings are unchanged; another core declares more bytes and buttons.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "util/Status.h"

namespace rn {

constexpr size_t kInputRecordBytes = 32;  // capacity of InputRecord; a layout uses recordBytes of them
constexpr size_t kInputEventsByte = 2;    // byte 2 is always the system events (kEvSoftReset, ...)

// Bytes 0, 1, 2 are p1, p2 and events (so {p1, p2, events} initialises an NES record); bytes
// 3.. are `more` (zero unless the layout uses them).
struct InputRecord {
  uint8_t p1 = 0, p2 = 0, events = 0;
  uint8_t more[kInputRecordBytes - 3] = {};
  uint8_t byte(size_t i) const { return i == 0 ? p1 : i == 1 ? p2 : i == 2 ? events : more[i - 3]; }
  void setByte(size_t i, uint8_t v) {
    if (i == 0) p1 = v;
    else if (i == 1) p2 = v;
    else if (i == 2) events = v;
    else more[i - 3] = v;
  }
  bool operator==(const InputRecord& o) const {
    if (p1 != o.p1 || p2 != o.p2 || events != o.events) return false;
    for (size_t i = 0; i < sizeof more; ++i)
      if (more[i] != o.more[i]) return false;
    return true;
  }
  bool operator!=(const InputRecord& o) const { return !(*this == o); }
};

struct InputLayout {
  struct Button {
    std::string id;       // action name, "p<port>.<name>": "p1.a", "p1.b1", "p1.coin"
    std::string label;    // display name; empty = the frontend's own name for the id
    uint16_t byte = 0;    // record byte (never kInputEventsByte)
    uint8_t bit = 0;      // 0..7
    std::string turboId;  // action that presses this button with turbo; empty = id + ".turbo"
  };
  struct Axis {
    std::string id;       // action name, "p<port>.axis.<name>": "p1.axis.x"
    std::string label;
    uint16_t byte = 0;    // int16 little-endian at [byte, byte + 1]
  };
  std::string name;       // "nes" for the NES layout; otherwise the core's name for it
  uint32_t recordBytes = 3;
  std::vector<Button> buttons;
  std::vector<Axis> axes;
  // Pairs of opposite directions (button ids) the SOCD policy resolves when both are pressed.
  std::vector<std::pair<std::string, std::string>> opposite;

  static const InputLayout& nes();
  bool isNes() const { return name == "nes"; }
  std::string turboOf(const Button& b) const { return b.turboId.empty() ? b.id + ".turbo" : b.turboId; }
  Status validate() const;
  std::string toJson() const;
  static Status fromJson(const std::string& json, InputLayout& out);
  bool operator==(const InputLayout& o) const { return toJson() == o.toJson(); }
  bool operator!=(const InputLayout& o) const { return !(*this == o); }
};

}  // namespace rn
