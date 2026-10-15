// Frontend core: CRT reception (no signal / power-on): the sequence, the receiver settings the CRT
// model renders it with, the tracker that tells which picture has which reception, and the hiss.
// Display / audio output only (nothing here touches a session).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

namespace {
rnf_crt_reception at(int step, uint64_t counter = 7, int reduced = 0) {
  rnf_crt_reception r;
  std::memset(&r, 0xAB, sizeof r);
  rnf_crt_reception_at(step, counter, reduced, &r);
  return r;
}
rnf_crt_receiver receiver(const rnf_crt_reception& r, double cnr = 58.0) {
  rnf_crt_receiver x;
  rnf_crt_reception_receiver(&r, cnr, &x);
  return x;
}
bool same(const rnf_crt_reception& a, const rnf_crt_reception& b) { return std::memcmp(&a, &b, sizeof a) == 0; }
}  // namespace

TEST_CASE("crt reception: normal is neutral") {
  for (int step : {RNF_CRT_RECEPTION_NORMAL, RNF_CRT_POWER_ON_FRAMES, RNF_CRT_POWER_ON_FRAMES + 5, -7}) {
    rnf_crt_reception r = at(step);
    CHECK(r.active == 0);
    CHECK(r.signal == 1 && r.chroma == 1 && r.picture == 1 && r.lift == 0 && r.hiss == 0);
    CHECK(r.roll == 0 && r.tear == 0 && r.jitter == 0);
    CHECK(rnf_crt_reception_deflects(&r) == 0);
    rnf_crt_receiver x = receiver(r);
    // The model's own defaults: GatedAGC 1/16 .. 16, carrier 1, decode unchanged.
    CHECK(x.signal_level == 1 && x.agc_min_gain == 1.0f / 16 && x.agc_max_gain == 16 && x.chroma == 1 && x.lift == 0);
  }
}

TEST_CASE("crt reception: no signal is snow (noise only, AGC to its maximum, colour killed)") {
  rnf_crt_reception r = at(RNF_CRT_RECEPTION_NO_SIGNAL, 42);
  CHECK(r.active == 1 && r.no_signal == 1);
  CHECK(r.signal == 0 && r.chroma == 0 && r.noise > 0.2f && r.lift > 0);
  CHECK(r.noise_frame == 42u);  // animated: a new noise field per picture
  CHECK(r.hiss == 0);           // the steady no-signal picture is silent (shown paused)
  CHECK(rnf_crt_reception_deflects(&r) == 0);
  rnf_crt_receiver x = receiver(r, 58.0);
  CHECK(x.signal_level == 0);
  // Maximum gain brings the receiver noise (10^(-58/20) of the carrier) to the snow level.
  CHECK(x.agc_max_gain > 200 && x.agc_max_gain < 600);
  CHECK(x.agc_min_gain == 1.0f / 16);
  // A weaker antenna has more noise: less gain for the same snow, never below 2.
  CHECK(receiver(r, 30.0).agc_max_gain < x.agc_max_gain);
  CHECK(receiver(r, 0.0).agc_max_gain == 2.0f);
  // Deterministic: a pure function of (step, counter, flash).
  CHECK(same(at(RNF_CRT_RECEPTION_NO_SIGNAL, 42), r));
}

TEST_CASE("crt reception: power-on sequence") {
  const int n = RNF_CRT_POWER_ON_FRAMES, snow = RNF_CRT_POWER_ON_SNOW_FRAMES;
  CHECK(n >= 50 && n <= 70);  // ~1 s at 60.1 Hz
  for (int s = 0; s < snow; s++) {
    rnf_crt_reception r = at(s, 100 + s);
    CHECK(r.active && r.no_signal && r.signal == 0 && r.hiss == 1 && r.noise_frame == uint32_t(100 + s));
  }
  float lastSignal = 0, lastHiss = 1, lastTear = 1e9f, lastRoll = 1e9f, lastChroma = 0;
  for (int s = snow; s < n; s++) {
    rnf_crt_reception r = at(s);
    CHECK(r.active && !r.no_signal);
    CHECK(r.signal >= lastSignal && r.signal > 0 && r.signal <= 1);
    CHECK(r.hiss <= lastHiss);
    CHECK(r.tear <= lastTear && r.roll <= lastRoll && r.roll >= 0);
    CHECK(r.chroma >= lastChroma);
    lastSignal = r.signal, lastHiss = r.hiss, lastTear = r.tear, lastRoll = r.roll, lastChroma = r.chroma;
  }
  // Locked by the end: full carrier, colour on, deflection within a fraction of a line / sample.
  rnf_crt_reception end = at(n - 1);
  CHECK(end.signal == 1 && end.chroma == 1 && end.hiss == 0 && end.lift == 0);
  CHECK(end.roll < 0.5f && end.tear < 0.5f && end.jitter < 0.1f);
  // The vertical roll passes more than a whole screen (the picture rolls, then catches).
  CHECK(at(snow).roll > 262);
  CHECK(rnf_crt_reception_deflects(&end) == 1);
  // The carrier is up within ~10 frames while the gated AGC may still go up to the snow gain.
  CHECK(at(snow + 9).signal == 1);
  rnf_crt_receiver x = receiver(at(snow + 3));
  CHECK(x.agc_max_gain > 200 && x.signal_level == at(snow + 3).signal && x.agc_min_gain == 1.0f / 16);
}

TEST_CASE("crt reception: flash reduction = static low-contrast grain, monotonic fade") {
  const int n = RNF_CRT_POWER_ON_FRAMES, snow = RNF_CRT_POWER_ON_SNOW_FRAMES;
  rnf_crt_reception full = at(RNF_CRT_RECEPTION_NO_SIGNAL, 5, 0), red = at(RNF_CRT_RECEPTION_NO_SIGNAL, 5, 1);
  CHECK(red.noise < full.noise * 0.4f);  // low contrast
  CHECK(red.lift <= full.lift);
  // Static: the same noise field whatever the picture counter.
  CHECK(red.noise_frame == at(RNF_CRT_RECEPTION_NO_SIGNAL, 999, 1).noise_frame);
  CHECK(same(at(3, 1, 1), at(3, 2, 1)));
  CHECK(red.agc_pinned == 1);
  // The fade: no deflection ever; picture level up, pinned gain (grain) and lift down, monotonic.
  double lastLevel = -1, lastGain = 1e30, lastLift = 1e9;
  for (int s = 0; s < n; s++) {
    rnf_crt_reception r = at(s, uint64_t(s), 1);
    CHECK(r.roll == 0 && r.tear == 0 && r.jitter == 0 && rnf_crt_reception_deflects(&r) == 0);
    CHECK(r.agc_pinned == 1);
    rnf_crt_receiver x = receiver(r);
    CHECK(x.agc_min_gain == x.agc_max_gain);            // pinned
    double level = double(x.signal_level) * x.agc_max_gain;  // picture level = carrier x gain
    CHECK(level >= lastLevel - 1e-6);
    CHECK(x.agc_max_gain <= lastGain);
    CHECK(r.lift <= lastLift);
    if (s < snow) CHECK(level == 0);
    lastLevel = level, lastGain = x.agc_max_gain, lastLift = r.lift;
  }
  CHECK(std::fabs(lastLevel - 1) < 1e-6 && lastGain == 1 && lastLift == 0);
}

TEST_CASE("crt power tracker: power-on, resets, jumps") {
  rnf_crt_power* p = rnf_crt_power_new();
  CHECK(rnf_crt_power_current(p) == RNF_CRT_RECEPTION_NORMAL);
  // Frame 0: the machine is not powered yet = no signal.
  CHECK(rnf_crt_power_show(p, 0) == RNF_CRT_RECEPTION_NO_SIGNAL);
  CHECK(rnf_crt_power_tick(p) == RNF_CRT_RECEPTION_NO_SIGNAL);  // animated snow while paused
  uint64_t c = rnf_crt_power_counter(p);
  CHECK(c == 2);
  // The step from frame 0 is the power-on.
  CHECK(rnf_crt_power_step(p, 0, 1, 0) == 0);
  for (int i = 1; i < RNF_CRT_POWER_ON_FRAMES; i++) CHECK(rnf_crt_power_step(p, uint64_t(i), uint64_t(i + 1), 0) == i);
  CHECK(rnf_crt_power_step(p, 60, 61, 0) == RNF_CRT_RECEPTION_NORMAL);
  CHECK(rnf_crt_power_step(p, 61, 62, 0) == RNF_CRT_RECEPTION_NORMAL);
  CHECK(rnf_crt_power_counter(p) == c + RNF_CRT_POWER_ON_FRAMES + 2);
  // A soft reset / power cycle applied on a step restarts it.
  CHECK(rnf_crt_power_step(p, 500, 501, RN_EV_SOFT_RESET) == 0);
  CHECK(rnf_crt_power_step(p, 501, 502, 0) == 1);
  CHECK(rnf_crt_power_step(p, 502, 503, RN_EV_POWER_CYCLE) == 0);
  // A jump (seek, rewind) ends it; a jump to frame 0 is no signal again.
  CHECK(rnf_crt_power_show(p, 300) == RNF_CRT_RECEPTION_NORMAL);
  CHECK(rnf_crt_power_step(p, 300, 301, 0) == RNF_CRT_RECEPTION_NORMAL);
  CHECK(rnf_crt_power_show(p, 0) == RNF_CRT_RECEPTION_NO_SIGNAL);
  // Practice (frame unknown): only the events count.
  CHECK(rnf_crt_power_show(p, 40) == RNF_CRT_RECEPTION_NORMAL);
  CHECK(rnf_crt_power_step(p, UINT64_MAX, 40, 0) == RNF_CRT_RECEPTION_NORMAL);
  // Export intro: start() makes the next picture step 0 wherever the range starts.
  rnf_crt_power_start(p);
  CHECK(rnf_crt_power_show(p, 1234) == 0);
  CHECK(rnf_crt_power_step(p, 1234, 1235, 0) == 1);
  rnf_crt_power_start(p);
  CHECK(rnf_crt_power_step(p, 99, 100, 0) == 0);
  rnf_crt_power_reset(p);
  CHECK(rnf_crt_power_current(p) == RNF_CRT_RECEPTION_NORMAL && rnf_crt_power_counter(p) == 0);
  rnf_crt_power_free(p);
}

TEST_CASE("crt hiss: deterministic, fades, leaves silence alone when off") {
  std::vector<int16_t> a(800, 1000), b(800, 1000);
  rnf_crt_hiss* h1 = rnf_crt_hiss_new();
  rnf_crt_hiss* h2 = rnf_crt_hiss_new();
  // Off: untouched.
  rnf_crt_hiss_mix(h1, a.data(), a.size(), 0);
  CHECK(a == std::vector<int16_t>(800, 1000));
  // Same calls, same samples (export reproducibility).
  rnf_crt_hiss_mix(h1, a.data(), a.size(), 1);
  rnf_crt_hiss_mix(h2, b.data(), b.size(), 1);
  CHECK(a == b);
  CHECK(rnf_crt_hiss_level(h1) == 1.0f);
  // Full hiss: the game sound is gone (ramps in from 0 over the first call), noise of modest level.
  std::vector<int16_t> c(4000, 1000);
  rnf_crt_hiss_mix(h1, c.data(), c.size(), 1);
  double mean = 0, sq = 0;
  int peak = 0;
  for (int16_t v : c) mean += v, sq += double(v) * v, peak = std::max(peak, std::abs(int(v)));
  mean /= c.size();
  double rms = std::sqrt(sq / c.size() - mean * mean);
  CHECK(std::fabs(mean) < 300);                              // game DC (1000) replaced by noise
  CHECK(rms > 0.03 * 32767 && rms < 0.12 * 32767);           // modest volume
  CHECK(peak < 32767);
  // Fading out to 0 ends exactly on the game sound.
  std::vector<int16_t> d(800, 1000);
  rnf_crt_hiss_mix(h1, d.data(), d.size(), 0);
  CHECK(d.back() == 1000);
  CHECK(rnf_crt_hiss_level(h1) == 0.0f);
  rnf_crt_hiss_reset(h2);
  CHECK(rnf_crt_hiss_level(h2) == 0.0f);
  rnf_crt_hiss_free(h1);
  rnf_crt_hiss_free(h2);
}
