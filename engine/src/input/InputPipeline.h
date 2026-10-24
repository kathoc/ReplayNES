// InputPipeline: physical input -> remappable actions -> turbo / SOCD policy -> the game input of a
// frame (InputRecord, laid out by an InputLayout; default: the NES pads).
// Only the OUTPUT is recorded; mapping/turbo settings are never part of a take.
// Hotkeys live in a separate action namespace and can never reach the game input.
// Thread-safe: all methods lock an internal mutex, so device callbacks (UI thread) and the
// emulation thread (sample*) may call concurrently.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "input/InputLayout.h"
#include "util/Status.h"

namespace rn {

enum class Socd : int { Neutral = 0, LastWins = 1, Allow = 2 };

enum Hotkey : int {
  HK_PAUSE = 0, HK_FRAME_ADVANCE, HK_REWIND, HK_SLOW, HK_BOOKMARK, HK_SOFT_RESET, HK_POWER_CYCLE,
  HK_TOGGLE_MODE, HK_SAVE, HK_UNDO_TAKE, HK_FAST_FORWARD, HK_STEP_BACK, HK_MENU, HK_COUNT
};

// Actions: every button id of the layout ("p1.a", "p1.b1", "p1.coin"), its turbo action
// (InputLayout::turboOf: "p1.turbo_a" for the NES A button, "<id>.turbo" otherwise), every axis id
// ("p1.axis.x") and the hotkeys "hk.<name>".
class InputPipeline {
 public:
  // Replaces the layout. Bindings, latches and turbo phases of actions the new layout does not
  // have are dropped.
  Status setLayout(const InputLayout& layout);
  InputLayout layout() const;

  Status bind(const std::string& physical, const std::string& action);
  Status unbind(const std::string& physical, const std::string& action);  // action "" = all for physical
  void clearBindings();
  std::vector<std::pair<std::string, std::string>> bindings() const;

  Status setTurbo(uint32_t period, uint32_t duty);  // period>=1, 1<=duty<=period (frames on per period)
  // Turbo timing of one button (its turbo action uses it); period 0 = back to the global setting.
  Status setButtonTurbo(const std::string& button, uint32_t period, uint32_t duty);
  void setSocd(Socd s);
  Status setAnalogThreshold(float t);               // 0 < t < 1

  void setPressed(const std::string& physical, bool pressed);
  // Analog stick: virtual digital ids "<stick>.left/.right/.up/.down" (y > 0 = up) and analog
  // sources "<stick>.x", "<stick>.y", "<stick>.-x", "<stick>.-y" (the last two inverted) for axes.
  void setAxis(const std::string& stick, float x, float y);
  // Any other analog source (trigger, dial): value in -1..1, bindable to axis actions.
  void setAxisValue(const std::string& source, float value);
  void releasePrefix(const std::string& prefix);  // e.g. controller disconnected: "gc0:"
  void releaseAll();

  // Game input for logical frame `frame`. Taps shorter than a frame are latched for one sample.
  void sampleRecord(uint64_t frame, InputRecord& out);
  void sampleGame(uint64_t frame, uint8_t& p1, uint8_t& p2);  // the NES pads of sampleRecord
  // Hotkey edges since the last poll (bit = Hotkey) and currently held hotkeys.
  void pollHotkeys(uint32_t& pressedEdges, uint32_t& held);

  std::string toJson() const;
  Status fromJson(const std::string& json);

 private:
  struct Timing { uint32_t period, duty; };
  bool knownAction(const std::string& action) const;
  void setPressedLocked(const std::string& physical, bool pressed);
  bool actionHeld(const std::string& action, uint64_t* lastSeq) const;
  float axisValue(const std::string& action) const;

  mutable std::mutex m_;
  InputLayout layout_ = InputLayout::nes();
  std::multimap<std::string, std::string> bind_;     // physical -> action
  std::map<std::string, uint64_t> pressed_;          // physical -> press sequence number
  std::map<std::string, float> analog_;              // analog source -> value
  uint64_t seq_ = 0;
  uint32_t period_ = 2, duty_ = 1;
  std::map<std::string, Timing> buttonTurbo_;        // button id -> its own turbo timing
  Socd socd_ = Socd::Neutral;
  float threshold_ = 0.5f;
  std::map<std::string, uint64_t> latched_;          // game/turbo actions pressed since the last sample
  uint32_t hkEdges_ = 0;
  std::map<std::string, uint64_t> turboStart_;       // turbo action -> frame its phase started
};

}  // namespace rn
