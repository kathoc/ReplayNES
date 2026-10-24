#include "support/rn_test.h"
#include "core/ReplayNEScoreAudio.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace {
using rn::replaynes_audio::Pipeline;
using rn::replaynes_audio::State;

struct Oracle {
  uint64_t phase{};
  int64_t area{};
  int32_t dcInput{};
  int32_t dcOutput{};
  uint64_t cursor{};
  std::vector<int16_t> output;

  static int32_t average(int64_t value) {
    const int64_t half=int64_t(Pipeline::kOutputWidth/2U);
    return value>=0 ? int32_t((value+half)/int64_t(Pipeline::kOutputWidth))
                    : -int32_t((-value+half)/int64_t(Pipeline::kOutputWidth));
  }
  void push(int16_t value) {
    uint64_t begin=cursor*Pipeline::kOutputWidth+phase;
    const uint64_t end=begin+Pipeline::kInputWidth;
    while(begin<end) {
      const uint64_t boundary=(cursor+1U)*Pipeline::kOutputWidth;
      const uint64_t stop=std::min(end,boundary);
      area+=int64_t(value)*int64_t(stop-begin);
      phase+=stop-begin; begin=stop;
      if(begin==boundary) {
        const int32_t input=average(area);
        const int64_t recurrence=int64_t(input)-dcInput+
          int64_t(Pipeline::kDcNumerator)*dcOutput/Pipeline::kDcDenominator;
        const int32_t filtered=int32_t(std::clamp<int64_t>(recurrence,-32768,32767));
        output.push_back(int16_t(filtered)); dcInput=input; dcOutput=filtered;
        ++cursor; phase=0; area=0;
      }
    }
  }
};

std::vector<int16_t> pattern() {
  std::vector<int16_t> values(3000);
  for(size_t i=0;i<values.size();++i) {
    if(i<200) values[i]=0;
    else if(i==200) values[i]=32767;
    else if(i<1400) values[i]=12000;
    else values[i]=(i&1U)?int16_t(-18000):int16_t(9000);
  }
  return values;
}
}

TEST_CASE("ReplayNESCore continuous audio matches an independent integer area oracle") {
  const auto input=pattern(); Oracle oracle; Pipeline pipeline; std::vector<int16_t> actual;
  for(const auto sample:input) { oracle.push(sample); REQUIRE(pipeline.push(sample,actual)); }
  CHECK(actual==oracle.output);
  CHECK_EQ(pipeline.state().phase,oracle.phase);
  CHECK_EQ(pipeline.state().area,oracle.area);
  CHECK_EQ(pipeline.state().outputCursor,oracle.cursor);
  CHECK_EQ(pipeline.state().dcInput,oracle.dcInput);
  CHECK_EQ(pipeline.state().dcOutput,oracle.dcOutput);
}

TEST_CASE("ReplayNESCore continuous audio is invariant across blocks and restore") {
  const auto input=pattern();
  Pipeline singles,blocks,restored; std::vector<int16_t> a,b,c;
  for(const auto value:input) REQUIRE(singles.push(value,a));
  REQUIRE(blocks.push(input.data(),137,b));
  REQUIRE(blocks.push(input.data()+137,911,b));
  const State checkpoint=blocks.state();
  REQUIRE(restored.restore(checkpoint)); c=b;
  REQUIRE(blocks.push(input.data()+1048,input.size()-1048,b));
  REQUIRE(restored.push(input.data()+1048,input.size()-1048,c));
  CHECK(a==b); CHECK(b==c); CHECK(blocks.state()==restored.state());
}

TEST_CASE("ReplayNESCore audio profile has deterministic DC step and rejects corrupt state") {
  Pipeline dc,step; std::vector<int16_t> dcOut,stepOut;
  std::vector<int16_t> constant(4000,10000),edge(4000,0);
  std::fill(edge.begin()+1000,edge.end(),10000);
  REQUIRE(dc.push(constant.data(),constant.size(),dcOut));
  REQUIRE(step.push(edge.data(),edge.size(),stepOut));
  REQUIRE(dcOut.size()>20); REQUIRE(stepOut.size()>20);
  CHECK(dcOut.front()==10000); CHECK(std::abs(dcOut.back())<std::abs(dcOut.front()));
  CHECK(std::any_of(stepOut.begin(),stepOut.end(),[](int16_t v){return v>0;}));
  auto bad=dc.state(); bad.profile=2; CHECK_FALSE(step.restore(bad));
  bad=dc.state(); bad.phase=Pipeline::kOutputWidth; CHECK_FALSE(step.restore(bad));
  bad=dc.state(); bad.area=int64_t(bad.phase)*32768+1; CHECK_FALSE(step.restore(bad));
  bad=dc.state(); bad.dcOutput=32768; CHECK_FALSE(step.restore(bad));
  State cursorEdge{}; cursorEdge.phase=Pipeline::kOutputWidth-Pipeline::kInputWidth;
  cursorEdge.outputCursor=std::numeric_limits<uint64_t>::max();
  REQUIRE(step.restore(cursorEdge)); const State before=step.state();
  std::vector<int16_t> untouched{7}; CHECK_FALSE(step.push(1,untouched));
  CHECK(step.state()==before); CHECK(untouched==std::vector<int16_t>{7});
}

TEST_CASE("ReplayNESCore frame mapping preserves cross-frame area phase and DC against interval oracle") {
  // Build absolute slot intervals independently, then integrate each output
  // interval by intersection. The oracle never calls a Pipeline helper.
  constexpr uint64_t frameWidth=655171ULL*48000ULL;
  constexpr uint64_t sampleWidth=39375000ULL;
  std::vector<uint64_t> boundaries{0};
  std::vector<int16_t> values,actual,expected,restoredOutput;
  Pipeline pipeline,restored;
  int32_t previousInput=0,previousOutput=0;
  auto integral=[&](uint64_t begin,uint64_t end) {
    int64_t area=0;
    size_t slot=size_t(std::upper_bound(boundaries.begin(),boundaries.end(),begin)-
      boundaries.begin()-1);
    for(;begin<end;++slot) {
      const uint64_t stop=std::min(end,boundaries[slot+1]);
      area+=int64_t(values[slot])*int64_t(stop-begin);
      begin=stop;
    }
    return area;
  };
  for(unsigned frame=0;frame<12;++frame) {
    // Rendered and unrendered frame cadences both contain 29780/29781 slots.
    const size_t slots=(frame%5==0 || frame%5==2)?29780:29781;
    std::vector<int16_t> input(slots);
    for(size_t i=0;i<slots;++i) {
      input[i]=int16_t(int((i*7919U+frame*1237U)%65536U)-32768);
      values.push_back(input[i]);
      boundaries.push_back(uint64_t(frame)*frameWidth+(i+1)*frameWidth/slots);
    }
    REQUIRE(pipeline.pushFrame(input.data(),input.size(),actual));
    const uint64_t end=(frame+1)*frameWidth;
    while(expected.size()<end/sampleWidth) {
      const uint64_t begin=expected.size()*sampleWidth;
      const int64_t area=integral(begin,begin+sampleWidth);
      const int32_t averaged=area>=0 ? int32_t((area+int64_t(sampleWidth/2))/int64_t(sampleWidth))
        : -int32_t((-area+int64_t(sampleWidth/2))/int64_t(sampleWidth));
      const int32_t filtered=int32_t(std::clamp<int64_t>(int64_t(averaged)-previousInput+
        int64_t(65372)*previousOutput/65536,-32768,32767));
      expected.push_back(int16_t(filtered)); previousInput=averaged; previousOutput=filtered;
    }
    CHECK(actual==expected);
    CHECK_EQ(pipeline.state().phase,end%sampleWidth);
    CHECK_EQ(pipeline.state().area,integral(end/sampleWidth*sampleWidth,end));
    CHECK_EQ(pipeline.state().outputCursor,end/sampleWidth);
    CHECK_EQ(pipeline.state().dcInput,previousInput);
    CHECK_EQ(pipeline.state().dcOutput,previousOutput);
    CHECK_EQ(pipeline.state().input,input.back());
    CHECK(pipeline.state().hasInput);
    if(frame==4) {
      CHECK(pipeline.state().phase!=0);
      CHECK(pipeline.state().area!=0);
      REQUIRE(restored.restore(pipeline.state())); restoredOutput=actual;
    } else if(frame>4) {
      REQUIRE(restored.pushFrame(input.data(),input.size(),restoredOutput));
      CHECK(restoredOutput==actual); CHECK(restored.state()==pipeline.state());
    }
  }
}

TEST_CASE("ReplayNESCore frame mapping rejects invalid input atomically") {
  Pipeline pipeline; std::vector<int16_t> output;
  const std::vector<int16_t> input(29781,1234);
  REQUIRE(pipeline.pushFrame(input.data(),input.size(),output));
  const auto before=pipeline.state(); const auto beforeOutput=output;
  CHECK_FALSE(pipeline.pushFrame(nullptr,0,output));
  CHECK_FALSE(pipeline.pushFrame(input.data(),0,output));
  CHECK_FALSE(pipeline.pushFrame(nullptr,29781,output));
  CHECK_FALSE(pipeline.pushFrame(input.data(),29782,output));
  CHECK(pipeline.state()==before); CHECK(output==beforeOutput);
  auto overflow=before;
  overflow.outputCursor=std::numeric_limits<uint64_t>::max()-1;
  REQUIRE(pipeline.restore(overflow));
  CHECK_FALSE(pipeline.pushFrame(input.data(),input.size(),output));
  CHECK(pipeline.state()==overflow); CHECK(output==beforeOutput);
}
