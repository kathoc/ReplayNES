// InputPipeline: physical input -> remappable actions -> turbo / SOCD policy -> final NES bitfields.
// Only the OUTPUT (p1/p2 bitfields) is recorded; mapping/turbo settings are never part of a take.
// Hotkeys live in a separate action namespace and can never reach the game bitfields.
// Thread-safe: all methods lock an internal mutex, so device callbacks (UI thread) and the
// emulation thread (sample*) may call concurrently.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "util/Status.h"

namespace rn {

enum class Socd : int { Neutral = 0, LastWins = 1, Allow = 2 };

// Action ids. Game buttons: player*8 + bit (bit order = NES: A,B,Select,Start,Up,Down,Left,Right).
enum : int {
  kActP1Base = 0,
  kActP2Base = 8,
  kActP1TurboA = 16, kActP1TurboB = 17, kActP2TurboA = 18, kActP2TurboB = 19,
  kActHotkeyBase = 32,  // + hotkey index
};
enum Hotkey : int {
  HK_PAUSE = 0, HK_FRAME_ADVANCE, HK_REWIND, HK_SLOW, HK_BOOKMARK, HK_SOFT_RESET, HK_POWER_CYCLE,
  HK_TOGGLE_MODE, HK_SAVE, HK_UNDO_TAKE, HK_FAST_FORWARD, HK_STEP_BACK, HK_COUNT
};

bool actionFromName(const std::string& name, int& action);
std::string actionName(int action);

class InputPipeline {
 public:
  Status bind(const std::string& physical, const std::string& action);
  Status unbind(const std::string& physical, const std::string& action);  // action "" = all for physical
  void clearBindings();
  std::vector<std::pair<std::string, std::string>> bindings() const;

  Status setTurbo(uint32_t period, uint32_t duty);  // period>=1, 1<=duty<=period (frames on per period)
  void setSocd(Socd s);
  Status setAnalogThreshold(float t);               // 0 < t < 1

  void setPressed(const std::string& physical, bool pressed);
  // Analog stick -> virtual ids "<stick>.left/.right/.up/.down" (y > 0 = up).
  void setAxis(const std::string& stick, float x, float y);
  void releasePrefix(const std::string& prefix);  // e.g. controller disconnected: "gc0:"
  void releaseAll();

  // Game input for logical frame `frame`. Taps shorter than a frame are latched for one sample.
  void sampleGame(uint64_t frame, uint8_t& p1, uint8_t& p2);
  // Hotkey edges since the last poll (bit = Hotkey) and currently held hotkeys.
  void pollHotkeys(uint32_t& pressedEdges, uint32_t& held);

  std::string toJson() const;
  Status fromJson(const std::string& json);

 private:
  void setPressedLocked(const std::string& physical, bool pressed);
  bool actionHeld(int action, uint64_t* lastSeq) const;

  mutable std::mutex m_;
  std::multimap<std::string, int> bind_;
  std::map<std::string, uint64_t> pressed_;  // physical -> press sequence number
  uint64_t seq_ = 0;
  uint32_t period_ = 2, duty_ = 1;
  Socd socd_ = Socd::Neutral;
  float threshold_ = 0.5f;
  uint64_t latch_ = 0;           // actions pressed since last game sample (bit = action < 32)
  uint64_t latchSeq_[32] = {};   // press seq of latched actions
  uint32_t hkEdges_ = 0;
  std::map<int, uint64_t> turboStart_;
};

}  // namespace rn
