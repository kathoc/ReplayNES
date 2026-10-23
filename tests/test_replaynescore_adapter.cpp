#include "support/rn_test.h"
#include "support/TestUtil.h"
#include "core/ReplayNEScoreAdapter.h"
#include <algorithm>
#include <array>
#include <vector>
#include <fstream>
namespace {
std::vector<uint8_t> rom(std::initializer_list<uint8_t> code) {
  std::vector<uint8_t> out(16+16384+8192,0);
  out[0]='N';out[1]='E';out[2]='S';out[3]=26;out[4]=1;out[5]=1;
  std::copy(code.begin(),code.end(),out.begin()+16);
  for(auto offset:{0x3ffa,0x3ffc,0x3ffe}) out[16+offset+1]=0x80;
  return out;
}
}
TEST_CASE("Experimental core real pad bus, PCM contract, bounded MMIO fault") {
  rn::ReplayNEScoreAdapter core;
  // Strobe then read both pad ports into RAM, loop. A is bit 0.
  auto bytes=rom({0xa9,1,0x8d,0x16,0x40,0xa9,0,0x8d,0x16,0x40,
    0xad,0x16,0x40,0x85,0,0xad,0x17,0x40,0x85,1,0x4c,0,0x80});
  REQUIRE(core.loadROM(bytes.data(),bytes.size()).ok());
  REQUIRE(core.stepFrame(1,0).ok());
  CHECK_EQ(core.cpuRam()[0]&1,1); CHECK_EQ(core.cpuRam()[1]&1,0);
  REQUIRE(core.stepFrame(0,1,false).ok());
  CHECK_EQ(core.cpuRam()[0]&1,0); CHECK_EQ(core.cpuRam()[1]&1,1);
  size_t count=999; CHECK(core.audio(&count)!=nullptr); CHECK_EQ(count,size_t(rn::audioSamplesForFrame(1)));
  std::vector<uint8_t> state; REQUIRE(core.saveState(state).ok()); CHECK(state.size()>256U*240U*4U);
  rn::ReplayNEScoreAdapter restored; REQUIRE(restored.loadROM(bytes.data(),bytes.size()).ok());
  REQUIRE(restored.loadState(state.data(),state.size()).ok()); CHECK_EQ(restored.frameIndex(),core.frameIndex());
  size_t loadedAudio=99; CHECK(restored.audio(&loadedAudio)==nullptr); CHECK_EQ(loadedAudio,0U);
  CHECK(std::equal(core.video(),core.video()+rn::kVideoWidth*rn::kVideoHeight,restored.video()));
  auto corrupt=state; corrupt.back()^=1; const auto before=restored.frameIndex();
  CHECK_EQ(restored.loadState(corrupt.data(),corrupt.size()).code,rn::Err::StateError);
  CHECK_EQ(restored.frameIndex(),before); CHECK_EQ(core.softReset().code,rn::Err::UnsupportedFormat);
  REQUIRE(core.powerCycle().ok()); CHECK_EQ(core.frameIndex(),2U);
  REQUIRE(core.stepFrame(0,0).ok()); CHECK_EQ(core.frameIndex(),3U);
  // Both control registers work; disabling channels preserves their settings.
  bytes=rom({0xa9,0xbf,0x8d,0x00,0x40,0xa9,0x65,0x8d,0x04,0x40,
    0xa9,0,0x8d,0x15,0x40,0x4c,0,0x80});
  REQUIRE(core.loadROM(bytes.data(),bytes.size()).ok());
  REQUIRE(core.stepFrame(0,0).ok());
  REQUIRE(core.stepFrame(0,0).ok());
  // $4015 status exposes a live pulse length counter instead of faulting.
  bytes=rom({0xa9,1,0x8d,0x15,0x40,0xa9,0,0x8d,0x03,0x40,
    0xad,0x15,0x40,0x85,0,0x4c,0x0f,0x80});
  REQUIRE(core.loadROM(bytes.data(),bytes.size()).ok());
  REQUIRE(core.stepFrame(0,0).ok());
  CHECK_EQ(core.cpuRam()[0]&1,1);
  // Pulse and implemented triangle registers have stateful side effects; the triangle gap remains unsupported.
  bytes=rom({0xa9,0,
    0x8d,0x00,0x40,0x8d,0x01,0x40,0x8d,0x02,0x40,0x8d,0x03,0x40,
    0x8d,0x04,0x40,0x8d,0x05,0x40,0x8d,0x06,0x40,0x8d,0x07,0x40,
    0x8d,0x08,0x40,0x8d,0x0a,0x40,0x8d,0x0b,0x40,
    0x8d,0x09,0x40,0x4c,0,0x80});
  REQUIRE(core.loadROM(bytes.data(),bytes.size()).ok());
  const auto failure=core.stepFrame(0,0);
  CHECK_FALSE(failure.ok()); CHECK(failure.message.find("$4009")!=std::string::npos);
  CHECK_EQ(core.frameIndex(),0U); CHECK_EQ(core.stepFrame(0,0).message,failure.message);
}
TEST_CASE("Experimental core emits deterministic non-silent pulse PCM") {
  // Enable pulse 1, constant volume, duty 50%, period $100, long length, then loop.
  auto bytes=rom({0xa9,1,0x8d,0x15,0x40,0xa9,0x9f,0x8d,0x00,0x40,
    0xa9,0,0x8d,0x02,0x40,0xa9,9,0x8d,0x03,0x40,0x4c,0x14,0x80});
  rn::ReplayNEScoreAdapter a,b;
  REQUIRE(a.loadROM(bytes.data(),bytes.size()).ok()); REQUIRE(b.loadROM(bytes.data(),bytes.size()).ok());
  for(unsigned frame=0;frame<3;++frame) {
    REQUIRE(a.stepFrame(0,0,false).ok()); REQUIRE(b.stepFrame(0,0,false).ok());
    size_t an=0,bn=0; const auto* ap=a.audio(&an); const auto* bp=b.audio(&bn);
    REQUIRE_EQ(an,size_t(rn::audioSamplesForFrame(frame))); REQUIRE_EQ(an,bn);
    CHECK(std::equal(ap,ap+an,bp));
    CHECK(std::any_of(ap,ap+an,[](int16_t value){return value!=0;}));
  }
}
TEST_CASE("Experimental core restores noise PCM into a separate instance") {
  // Enable noise, use constant volume and the shortest period, read status, then loop.
  auto bytes=rom({0xa9,8,0x8d,0x15,0x40,0xa9,0x3f,0x8d,0x0c,0x40,
    0xa9,0,0x8d,0x0e,0x40,0xa9,0xf8,0x8d,0x0f,0x40,
    0xad,0x15,0x40,0x85,0,0x4c,0x19,0x80});
  rn::ReplayNEScoreAdapter straight,branch;
  REQUIRE(straight.loadROM(bytes.data(),bytes.size()).ok()); REQUIRE(branch.loadROM(bytes.data(),bytes.size()).ok());
  for(unsigned frame=0;frame<3;++frame) REQUIRE(straight.stepFrame(0,0,false).ok());
  CHECK_EQ(straight.cpuRam()[0]&8,8);
  size_t initialCount=0;const auto* initial=straight.audio(&initialCount);
  CHECK(std::any_of(initial,initial+initialCount,[](int16_t value){return value!=0;}));
  std::vector<uint8_t> state; REQUIRE(straight.saveState(state).ok());
  REQUIRE(branch.loadState(state.data(),state.size()).ok()); CHECK_EQ(branch.frameIndex(),3U);
  for(unsigned frame=3;frame<7;++frame) {
    REQUIRE(straight.stepFrame(0,0,false).ok()); REQUIRE(branch.stepFrame(0,0,false).ok());
    size_t an=0,bn=0; const auto* ap=straight.audio(&an); const auto* bp=branch.audio(&bn);
    REQUIRE_EQ(an,size_t(rn::audioSamplesForFrame(frame))); REQUIRE_EQ(an,bn);
    CHECK(std::equal(ap,ap+an,bp)); CHECK_EQ(straight.machineHash(),branch.machineHash());
  }
}
TEST_CASE("Experimental core restores DMC direct DAC PCM into a separate instance") {
  // Program DMC registers without starting sample DMA; direct DAC remains live and stateful.
  auto bytes=rom({0xa9,0,0x8d,0x10,0x40,0xa9,64,0x8d,0x11,0x40,
    0xa9,0x12,0x8d,0x12,0x40,0xa9,3,0x8d,0x13,0x40,
    0xad,0x15,0x40,0x85,0,0x4c,0x19,0x80});
  rn::ReplayNEScoreAdapter straight,branch;
  REQUIRE(straight.loadROM(bytes.data(),bytes.size()).ok()); REQUIRE(branch.loadROM(bytes.data(),bytes.size()).ok());
  REQUIRE(straight.stepFrame(0,0,false).ok()); CHECK_EQ(straight.cpuRam()[0]&0x90,0);
  size_t initialCount=0;const auto* initial=straight.audio(&initialCount);
  CHECK(std::any_of(initial,initial+initialCount,[](int16_t value){return value!=0;}));
  std::vector<uint8_t> state; REQUIRE(straight.saveState(state).ok());
  REQUIRE(branch.loadState(state.data(),state.size()).ok()); CHECK_EQ(branch.frameIndex(),1U);
  for(unsigned frame=1;frame<4;++frame) {
    REQUIRE(straight.stepFrame(0,0,false).ok()); REQUIRE(branch.stepFrame(0,0,false).ok());
    size_t an=0,bn=0;const auto* ap=straight.audio(&an);const auto* bp=branch.audio(&bn);
    REQUIRE_EQ(an,size_t(rn::audioSamplesForFrame(frame)));REQUIRE_EQ(an,bn);
    CHECK(std::equal(ap,ap+an,bp));CHECK_EQ(straight.machineHash(),branch.machineHash());
  }
}
TEST_CASE("Experimental core runs and restores DMC sample DMA") {
  // Inhibit frame IRQ, start a 17-byte fastest-rate DMC sample at $C000, then loop.
  auto bytes=rom({0xa9,0xc0,0x8d,0x17,0x40,0xa9,0x0f,0x8d,0x10,0x40,
    0xa9,0,0x8d,0x12,0x40,0xa9,1,0x8d,0x13,0x40,
    0xa9,0x10,0x8d,0x15,0x40,0x4c,0x19,0x80});
  rn::ReplayNEScoreAdapter straight,branch;
  REQUIRE(straight.loadROM(bytes.data(),bytes.size()).ok());
  REQUIRE(branch.loadROM(bytes.data(),bytes.size()).ok());
  REQUIRE(straight.stepFrame(0,0,false).ok());
  std::vector<uint8_t> state;REQUIRE(straight.saveState(state).ok());
  REQUIRE(branch.loadState(state.data(),state.size()).ok());
  for(unsigned frame=1;frame<4;++frame) {
    REQUIRE(straight.stepFrame(0,0,false).ok());REQUIRE(branch.stepFrame(0,0,false).ok());
    size_t an=0,bn=0;const auto* ap=straight.audio(&an);const auto* bp=branch.audio(&bn);
    REQUIRE_EQ(an,size_t(rn::audioSamplesForFrame(frame)));REQUIRE_EQ(an,bn);
    CHECK(std::equal(ap,ap+an,bp));CHECK_EQ(straight.machineHash(),branch.machineHash());
  }
}
TEST_CASE("Experimental core renders real PPU pattern with an exact raster frame") {
  auto bytes=rom({0xa9,0x3f,0x8d,6,0x20,0xa9,0,0x8d,6,0x20,
    0xa9,0x0f,0x8d,7,0x20,0xa9,0x21,0x8d,7,0x20,
    0xa9,0,0x8d,6,0x20,0x8d,6,0x20,
    0x2c,2,0x20,0x10,0xfb,0xa9,0x0a,0x8d,1,0x20,0x4c,38,0x80});
  for(unsigned row=0;row<8;++row) bytes[16+16384+row]=0xaa;
  rn::ReplayNEScoreAdapter core;
  REQUIRE(core.loadROM(bytes.data(),bytes.size()).ok());
  auto first=core.stepFrame(0,0); if(!first) MESSAGE(first.message); REQUIRE(first.ok());
  auto second=core.stepFrame(0,0); if(!second) MESSAGE(second.message); REQUIRE(second.ok());
  // At this deterministic power phase the first completed frame still polls vblank;
  // MASK becomes active late in frame 2. Frame 3 is the first full rendered picture.
  auto third=core.stepFrame(0,0); if(!third) MESSAGE(third.message); REQUIRE(third.ok());
  const auto* pixels=core.video();
  CHECK(pixels[100*256+100]!=pixels[100*256+101]);
  // PPM is the same BGRA buffer exposed to the frontend, not a reference-core picture.
  // tempDir honours $RN_TEST_TMP at run time (the Windows VM has no build-tree path).
  std::ofstream capture(rntest::tempDir("replaynescore-display")+"/replaynescore-display.ppm",std::ios::binary);
  capture<<"P6\n256 240\n255\n";
  for(unsigned n=0;n<256*240;++n) {
    const char rgb[]{char(pixels[n]>>16),char(pixels[n]>>8),char(pixels[n])};
    capture.write(rgb,3);
  }
  REQUIRE(bool(capture));
}
