// CRT reception around the picture: no signal (snow) and the power-on / reset transition (snow,
// then the carrier comes up, the AGC follows and the deflection locks), the hiss that goes with it,
// and the receiver settings the CRT model renders it with (docs/CRT_PORT.md "No signal and
// power-on"). Display / audio output only: nothing here reaches emulation, the input log or .nesrec.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "common.hpp"

namespace {

constexpr int kFrames = RNF_CRT_POWER_ON_FRAMES;
constexpr int kSnow = RNF_CRT_POWER_ON_SNOW_FRAMES;
constexpr int kLock = kFrames - kSnow;  // frames from the carrier coming up to the end

// Snow: receiver output noise (luma sigma, before the 2.2 power) at the maximum gain, and the
// detector's DC lift without a keyed clamp. Flash reduction: static, low-contrast grain.
constexpr float kNoise = 0.32f, kLift = 0.36f;
constexpr float kNoiseReduced = 0.07f, kLiftReduced = 0.22f;
// Luma sigma per unit RF noise sigma at unit AGC gain: the decode's factor (gain / ((0.875 / 1.052)
// * 0.788) = 1.526 gain) times the 12-sample average of the IF-filtered noise (~0.65).
constexpr double kLumaPerNoise = 0.99;
// Default GatedAGC limits (reference/agc.mjs; CRTAGC).
constexpr float kMinGain = 1.0f / 16.0f, kMaxGain = 16.0f;

void neutral(rnf_crt_reception* r) {
  *r = rnf_crt_reception{};
  r->signal = 1;
  r->chroma = 1;
  r->picture = 1;
}

void snow(rnf_crt_reception* r, uint64_t counter, bool reduced) {
  neutral(r);
  r->active = 1;
  r->no_signal = 1;
  r->signal = 0;
  r->noise = reduced ? kNoiseReduced : kNoise;
  r->lift = reduced ? kLiftReduced : kLift;
  r->agc_pinned = reduced ? 1 : 0;
  r->noise_frame = reduced ? 0u : uint32_t(counter);
  r->chroma = 0;
  r->picture = 0;
}

float clamp01(float x) { return std::min(1.0f, std::max(0.0f, x)); }

uint32_t hash32(uint32_t x) {  // lowbias32 (the CRT model's noise hash)
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

}  // namespace

extern "C" {

// Picture geometry (frontend.h "CRT picture geometry"): a dot's width / a line's height on a 4:3
// face whose width the 256 dots span `active_width` of and whose height the 240 lines span.
double rnf_crt_active_width(void) { return RNF_CRT_ACTIVE_WIDTH; }
double rnf_crt_pixel_aspect(double active_width) { return (4.0 / 3.0 * active_width / 256) / (1.0 / 240); }

void rnf_crt_reception_at(int step, uint64_t counter, int flash_reduced, rnf_crt_reception* out) {
  if (!out) return;
  const bool reduced = flash_reduced != 0;
  if (step == RNF_CRT_RECEPTION_NO_SIGNAL) {
    snow(out, counter, reduced);
    return;
  }
  if (step < 0 || step >= kFrames) {
    neutral(out);
    return;
  }
  snow(out, counter, reduced);
  out->hiss = 1;
  if (step < kSnow) return;
  const int k = step - kSnow;  // 0 .. kLock-1
  out->no_signal = 0;
  if (reduced) {
    // One monotonic fade: picture level b up, grain (pinned gain) and lift down, no deflection.
    float u = clamp01(float(k + 1) / float(kLock));
    float b = u * u * (3 - 2 * u);
    out->signal = b;
    out->lift = kLiftReduced * (1 - b);
    out->chroma = b;
    out->picture = b;
    out->hiss = 1 - b;
    return;
  }
  // The carrier comes up over ~10 frames (60 dB, logarithmic): the AGC follows it, so the picture
  // emerges from the snow; the clamp keys on the back porch again; the colour killer releases once
  // the burst is steady; the vertical roll and the horizontal tearing decay as the sync locks.
  const float up = clamp01(float(k + 1) / 10.0f);
  out->signal = std::pow(10.0f, -3.0f * (1 - up));
  out->lift = kLift * std::max(0.0f, 1.0f - float(k + 1) / 8.0f);
  out->chroma = clamp01(float(k - 8) / 6.0f);
  out->roll = 393.0f * std::exp(-float(k) / 4.5f);
  out->tear = 48.0f * std::exp(-float(k) / 6.0f);
  out->tear_cycles = 1.6f;
  out->tear_phase = 0.11f * float(k);
  out->jitter = 10.0f * std::exp(-float(k) / 5.0f);
  out->jitter_key = hash32(uint32_t(counter) ^ 0x5eed5eedu);
  out->picture = out->signal;
  out->hiss = std::max(0.0f, 1.0f - float(k) / 14.0f);
}

int rnf_crt_reception_deflects(const rnf_crt_reception* r) {
  if (!r || !r->active) return 0;
  return r->roll != 0 || r->tear != 0 || r->jitter != 0 ? 1 : 0;
}

void rnf_crt_reception_receiver(const rnf_crt_reception* r, double cnr_db, rnf_crt_receiver* out) {
  if (!out) return;
  *out = rnf_crt_receiver{1.0f, kMinGain, kMaxGain, 0u, 1.0f, 0.0f};
  if (!r || !r->active) return;
  // The gain that brings the receiver noise (sigma 10^(-CNR/20) of the carrier) to r->noise.
  double g = std::max(1.0, double(r->noise) / kLumaPerNoise * std::pow(10.0, cnr_db / 20.0));
  out->noise_frame = r->noise_frame;
  out->chroma = r->chroma;
  out->lift = r->lift;
  if (r->agc_pinned) {
    // Gain g^(1-b) and carrier b / gain: picture level b and noise both monotonic in b.
    double b = std::min(1.0, std::max(0.0, double(r->signal)));
    double gain = std::pow(g, 1.0 - b);
    out->signal_level = float(b / gain);
    out->agc_min_gain = out->agc_max_gain = float(gain);
  } else {
    out->signal_level = r->signal;
    out->agc_max_gain = float(std::max(2.0, g));
  }
}

}  // extern "C"

struct rnf_crt_power {
  int step = RNF_CRT_RECEPTION_NORMAL;
  uint64_t counter = 0;
  bool started = false;
};

struct rnf_crt_hiss {
  uint64_t n = 0;
  float level = 0;
  float lp = 0;
};

extern "C" {

rnf_crt_power* rnf_crt_power_new(void) { return new rnf_crt_power(); }
void rnf_crt_power_free(rnf_crt_power* p) { delete p; }
void rnf_crt_power_reset(rnf_crt_power* p) {
  if (p) *p = rnf_crt_power{};
}
void rnf_crt_power_start(rnf_crt_power* p) {
  if (p) p->started = true;
}

int rnf_crt_power_step(rnf_crt_power* p, uint64_t frame_before, uint64_t frame, uint8_t events) {
  if (!p) return RNF_CRT_RECEPTION_NORMAL;
  p->counter++;
  const bool power = frame_before == 0 || (events & (RN_EV_SOFT_RESET | RN_EV_POWER_CYCLE)) != 0;
  if (p->started || power) {
    p->started = false;
    p->step = 0;
  } else if (p->step >= 0) {
    p->step = p->step + 1 < kFrames ? p->step + 1 : RNF_CRT_RECEPTION_NORMAL;
  } else {
    p->step = RNF_CRT_RECEPTION_NORMAL;
  }
  (void)frame;
  return p->step;
}

int rnf_crt_power_show(rnf_crt_power* p, uint64_t frame) {
  if (!p) return RNF_CRT_RECEPTION_NORMAL;
  p->counter++;
  if (p->started) {
    p->started = false;
    p->step = 0;
  } else {
    p->step = frame == 0 ? RNF_CRT_RECEPTION_NO_SIGNAL : RNF_CRT_RECEPTION_NORMAL;
  }
  return p->step;
}

int rnf_crt_power_tick(rnf_crt_power* p) {
  if (!p) return RNF_CRT_RECEPTION_NORMAL;
  p->counter++;
  return p->step;
}

int rnf_crt_power_current(const rnf_crt_power* p) { return p ? p->step : RNF_CRT_RECEPTION_NORMAL; }
uint64_t rnf_crt_power_counter(const rnf_crt_power* p) { return p ? p->counter : 0; }

rnf_crt_hiss* rnf_crt_hiss_new(void) { return new rnf_crt_hiss(); }
void rnf_crt_hiss_free(rnf_crt_hiss* h) { delete h; }
void rnf_crt_hiss_reset(rnf_crt_hiss* h) {
  if (h) *h = rnf_crt_hiss{};
}
float rnf_crt_hiss_level(const rnf_crt_hiss* h) { return h ? h->level : 0.0f; }

void rnf_crt_hiss_mix(rnf_crt_hiss* h, int16_t* pcm, size_t n, float hiss) {
  if (!h) return;
  hiss = clamp01(hiss);
  if (!pcm || n == 0) {
    h->level = hiss;
    return;
  }
  if (h->level == 0 && hiss == 0) return;
  const float from = h->level;
  // Uniform white noise (two hashed uniforms per sample: triangular, fewer peaks), one-pole
  // low-pass (a TV speaker's "zaa", not a hard digital hiss); x1.6 restores the level it removes.
  const float amp = float(RNF_CRT_HISS_VOLUME) * 32767.0f * 1.6f;
  for (size_t i = 0; i < n; i++) {
    uint32_t a = hash32(uint32_t(h->n * 2) ^ 0x9e3779b9u), b = hash32(uint32_t(h->n * 2 + 1) ^ 0x7f4a7c15u);
    h->n++;
    float x = (float(a) + float(b)) / 4294967296.0f - 1.0f;  // -1 .. 1
    h->lp += 0.55f * (x - h->lp);
    float lvl = from + (hiss - from) * float(i + 1) / float(n);
    float v = float(pcm[i]) * (1 - lvl) + h->lp * amp * lvl;
    pcm[i] = int16_t(std::lround(std::min(32767.0f, std::max(-32768.0f, v))));
  }
  h->level = hiss;
}

}  // extern "C"
