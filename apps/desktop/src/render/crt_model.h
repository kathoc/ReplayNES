// Physical CRT model ported from nesterm (kathoc, MIT) vendor/crt -- itself exported from the
// "crt-physical-model" project (EXPORT-MANIFEST commit 919c1563). C++ port of the macOS app's
// apps/macos/Sources/Core/CRT/CRTModel.swift: the CPU-side SETUP math only (constants, FIR design,
// detector/beam tables, phosphor fractions), 1:1 (same formulas, evaluation order, double
// precision, float32 rounding points). Every per-frame operation runs on the GPU
// (apps/linux/shaders/crt/*.comp; Direct3D 11: the HLSL generated from them in
// apps/windows/shaders/crt). Uncalibrated, assumption-labelled model (ledger IDs kept in
// comments), not a measured television. Mapping and deviations: docs/CRT_PORT.md.
// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md
#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace rnl::crt {

// ------------------------------------------------------------------ reference/composite.mjs
namespace composite {
constexpr int kWidth = 256, kHeight = 240, kSamplesPerDot = 8, kLineSamples = 2728;
constexpr int kFrameSamples = 341 * 262 * 8;
constexpr double kSampleClockNumerator = 945000000.0, kSampleClockDenominator = 22.0;
constexpr int kCarrierSamples = 12;
constexpr int kActiveStart = 36, kActiveEnd = 2084, kSyncStart = 2244, kSyncEnd = 2444;
constexpr int kBurstStart = 2476, kBurstEnd = 2596, kClampStart = 2172, kClampEnd = 2244;
constexpr double kBlank = 0.312, kWhite = 1.100, kSync = 0.048, kBurstDC = 0.336;
constexpr double kBurstLow = 0.148, kBurstHigh = 0.524;
constexpr double kDriveExponent = 2.2;

inline int mod12(int n) { return ((n % 12) + 12) % 12; }
/// Composite voltage of a 9-bit PPU code at carrier phase (NES-M2-LEVELS).
double signalVoltage(int code, int phase);
/// rf-webgl.mjs voltageTexture: lut[phase*512 + code] (float32).
const std::vector<float>& voltageLUT();
/// Per-row carrier phase for a frame whose line 0 starts at `lineStart0` samples.
std::array<int32_t, 240> rowPhases(int lineStart0, int initialPhase = 0);
/// ReplayNES adapter: Nestopia colour-burst phase b -> row 0 sample start 36 + 4*b.
std::array<int32_t, 240> rowPhasesForBurst(uint32_t burstPhase);
}  // namespace composite

// ------------------------------------------------------------------ reference/rf.mjs
namespace rf {
constexpr double kModulationDepth = 0.875;
constexpr int kTaps = 1025;
constexpr double kVestigeHz = 750000.0, kCutoffHz = 4200000.0, kTransitionHz = 600000.0;
constexpr int kQuadratureBins = 4096;
constexpr int kMaxSamples = composite::kLineSamples * composite::kHeight;  // 654720
constexpr int kFFTSize = 4096;
constexpr int kOverlap = kTaps - 1;
constexpr int kHop = kFFTSize - kOverlap;
constexpr int kBlocks = (kMaxSamples + kHop - 1) / kHop;  // 214
double sampleRateHz();

struct Filter {
  std::vector<double> real, imag;
  int delaySamples = 0;
};
/// designIF(): asymmetric (vestigial) IF response (M3-IF). Cached.
const Filter& filter();
/// kernelSpectrum(): forward FFT (double) of the zero-padded FIR, float32 (re, im) pairs.
const std::vector<float>& kernelSpectrum();
/// Fast path: spectrum of Re(h) alone (the receiver reads only the real part of the IF output, and
/// for a real signal Re(signal * h) = signal * Re(h)). Float32 (re, im) pairs, natural order.
const std::vector<float>& kernelSpectrumReal();
/// twiddles: (cos, sin)(2*pi*i/N) as float32 pairs.
const std::vector<float>& twiddles();

constexpr double kNoiseBandwidthHz = 4.2e6, kNoiseFigureDb = 7.0, kDefaultInputDbuv = 65.0;
double carrierToNoiseDb(double inputDbuv);
double noiseSigma(double cnrDb, double signalLevel);
uint32_t hash32(uint32_t v);
int32_t noiseKey(uint32_t seed, uint64_t frame);
}  // namespace rf

// ------------------------------------------------------------------ reference/agc.mjs
namespace agc {
constexpr double kInitialGain = 1.0, kAttackSeconds = 0.002, kReleaseSeconds = 0.020;
constexpr double kMinGain = 1.0 / 16, kMaxGain = 16.0;
}  // namespace agc

// ------------------------------------------------------------------ reference/phosphor.mjs
namespace phosphor {
constexpr double kNesLineS = 341.0 * 4 / 21477272;
constexpr double kNesFrameS = 262 * kNesLineS;
constexpr double kCutoffS = 0.1;
std::vector<double> frameFractions(int channel, double framePeriod = kNesFrameS, double cutoff = kCutoffS);
const std::array<std::vector<double>, 3>& fractions();
int depth();  // 7
}  // namespace phosphor

// ------------------------------------------------------------------ reference/supply.mjs (M4C, assumed)
namespace supply {
constexpr double kV0 = 25000.0, kReff = 0.4e6, kTauS = 1.5e-3, kImax = 1.2e-3, kIlim = 0.8e-3, kTauAblS = 0.3, kN = 1.75;
constexpr double kShare[3] = {0.42, 0.30, 0.28};
constexpr double kLineS = phosphor::kNesLineS, kFrameS = phosphor::kNesFrameS;
constexpr double kVisibleLines = 240.0, kTotalLines = 262.0;
}  // namespace supply

// ------------------------------------------------------------------ reference/tube.mjs + spot-h.mjs
namespace tube {
constexpr double kSurfaceWidthM = 0.28, kSurfaceHeightM = 0.21;
constexpr double kTriadPitchM = 0.00075, kWidthFraction = 0.22, kVerticalPeriodM = 0.001, kVerticalFill = 0.8,
                 kStaggerFraction = 0.5;
constexpr double kSigmaXPitches = 0.42, kSigmaYPitches = 0.32, kCutoffSigma = 4.0;
constexpr double kGrowthDiscPitches = 1.22;
constexpr int kGrowthLevels = 33;
constexpr double kEmissionGain = 0.75;
constexpr double kScatterFraction = 0.12, kScatterSigmaM = 0.002;
constexpr double kAlbedo = 0.10, kDiffuseFraction = 0.65, kNormalSlopeX = 0.4, kNormalSlopeY = 0.3;
constexpr double kLightX = -0.3, kLightY = -0.4, kLightZ = 1.0, kReferenceWhiteCdM2 = 80.0;
constexpr double kReconstructionCutoffSigma = 4.0;
constexpr double kTruncatedGaussianMass = 0.9999366575163338;
constexpr int kSpotHRadius = 3;  // M4C-SPOT-H
double sigmaOutputPixels();

/// M4C-SPOT-H: extra horizontal sigma in drive samples for current fraction u.
double extraSigmaSamples(double u, double growth, int width, int scanLines = 240);

/// createTubePlan() result, packed for the GPU like tube-webgl.mjs (pairs = (index, weight)).
struct Plan {
  int width = 0, height = 0, outputWidth = 0, outputHeight = 0;
  double gain = 0, scatterFraction = 0;
  int xCount = 1;
  std::vector<float> xmap;  // [((x*6+b)*xCount + k)*2 + {0,1}]
  int yCount = 1;
  std::vector<float> ymap;  // fixed spot: [((y*2+parity)*yCount + k)*2 + {0,1}]
  int growthLevels = kGrowthLevels, gCount = 1;
  std::vector<float> gmap;    // [((y*2+parity)*(levels*gCount) + k*gCount + j)*2 + {0,1}]
  std::vector<float> colsum;  // [b*ow + x]
  int radius[2] = {0, 0};
  std::vector<float> kernel[2];  // scatter weights x, y
  std::vector<float> ambient;    // ow*oh
  bool growth = false;
};

/// createTubePlan(input {width 512, height lines, beamReference 256x240},
/// {outputWidth, outputHeight, samples 4, ambientLux, beamGrowth}) as used by physical-worker.mjs.
Plan makePlan(int lines, int outputWidth, int outputHeight, double ambientLux = 40, double beamGrowth = 1,
              int width = 512, int samples = 4, int beamReferenceWidth = 256, int beamReferenceHeight = 240);
/// Fast path tables (CRTTube.FastPlan): horizontal taps with padding at the tile's first input
/// (weight 0) + the input span (first, last) of every 64-column tile; vertical spot tables as
/// contiguous input rows (first, count) per output row and parity with a cubic in the beam-current
/// fraction per tap (fixed spot: the constant weight); 1 / colsum per (parity, column) as RGBA.
struct FastPlan {
  std::vector<float> xmap;      // same layout as Plan::xmap
  std::vector<int32_t> tiles;   // [tile*2 + {first, last}]
  int maxSpan = 0;
  std::vector<int32_t> vrows;   // [(y*2+parity)*2 + {first, count}]
  std::vector<float> vcoef;     // [((y*2+parity)*taps + k)*4 + {c0..c3}]
  int taps = 1;
  std::vector<float> colinv;    // [(parity*ow + x)*4 + c]
};
FastPlan fastPlan(const Plan& p);

/// Fast path scatter source (CRTTube.DriveScatter): drive-domain blur radii / weights per axis and
/// the mean emission per unit drive times the scatter fraction per phosphor.
struct DriveScatter {
  int rx = 0, ry = 0;
  std::vector<float> wx, wy;
  float kappa[3] = {0, 0, 0};
};
DriveScatter driveScatter(const Plan& p);
}  // namespace tube

}  // namespace rnl::crt
