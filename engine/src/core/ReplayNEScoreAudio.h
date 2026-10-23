#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rn::replaynes_audio {

struct State {
  static constexpr uint32_t kVersion = 1;
  static constexpr uint32_t kProfile = 1;
  uint32_t version{kVersion};
  uint32_t profile{kProfile};
  uint64_t phase{};
  int64_t area{};
  int32_t input{};
  bool hasInput{};
  uint64_t outputCursor{};
  int32_t dcInput{};
  int32_t dcOutput{};
  constexpr bool operator==(const State& other) const {
    return version==other.version && profile==other.profile && phase==other.phase &&
      area==other.area && input==other.input && hasInput==other.hasInput &&
      outputCursor==other.outputCursor && dcInput==other.dcInput && dcOutput==other.dcOutput;
  }
};

class Pipeline {
 public:
  static constexpr uint64_t kInputWidth = 22ULL * 48000ULL;
  static constexpr uint64_t kOutputWidth = 39375000ULL;
  static constexpr int32_t kDcNumerator = 65372;
  static constexpr int32_t kDcDenominator = 65536;

  const State& state() const { return state_; }

  static bool valid(const State& value) {
    if(value.version != State::kVersion || value.profile != State::kProfile ||
       value.phase >= kOutputWidth || value.input < -32768 || value.input > 32767 ||
       value.dcInput < -32768 || value.dcInput > 32767 ||
       value.dcOutput < -32768 || value.dcOutput > 32767)
      return false;
    if(!value.hasInput && value.input != 0) return false;
    const uint64_t magnitude = value.area < 0
      ? uint64_t(-(value.area + 1)) + 1U : uint64_t(value.area);
    return magnitude <= value.phase * uint64_t{32768};
  }

  bool restore(const State& value) {
    if(!valid(value)) return false;
    state_ = value;
    return true;
  }

  bool push(int16_t input, std::vector<int16_t>& output) {
    if(state_.outputCursor == std::numeric_limits<uint64_t>::max() &&
       state_.phase + kInputWidth >= kOutputWidth)
      return false;
    state_.input = input;
    state_.hasInput = true;
    uint64_t remaining = kInputWidth;
    while(remaining != 0) {
      const uint64_t width = std::min(remaining, kOutputWidth - state_.phase);
      state_.area += int64_t(input) * int64_t(width);
      state_.phase += width;
      remaining -= width;
      if(state_.phase == kOutputWidth) {
        const int32_t averaged = roundedAverage(state_.area);
        const int64_t recurrence = int64_t(averaged) - state_.dcInput +
          int64_t(kDcNumerator) * state_.dcOutput / kDcDenominator;
        const int32_t filtered = int32_t(std::clamp<int64_t>(recurrence, -32768, 32767));
        output.push_back(int16_t(filtered));
        ++state_.outputCursor;
        state_.dcInput = averaged;
        state_.dcOutput = filtered;
        state_.phase = 0;
        state_.area = 0;
      }
    }
    return true;
  }

  bool push(const int16_t* input, size_t count, std::vector<int16_t>& output) {
    if(!input && count!=0) return false;
    for(size_t i=0;i<count;++i) if(!push(input[i], output)) return false;
    return true;
  }

 private:
  static int32_t roundedAverage(int64_t area) {
    const int64_t half = int64_t(kOutputWidth / 2U);
    return area >= 0 ? int32_t((area + half) / int64_t(kOutputWidth))
                     : -int32_t((-area + half) / int64_t(kOutputWidth));
  }
  State state_{};
};

} // namespace rn::replaynes_audio
