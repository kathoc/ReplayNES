// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md
// 1:1 port of apps/macos/Sources/Core/CRT/CRTModel.swift (see crt_model.h).
#include "render/crt_model.h"

#include <algorithm>
#include <cmath>

namespace rnl::crt {

namespace {
constexpr double kPi = 3.141592653589793;
}

// ------------------------------------------------------------------ composite
namespace composite {
namespace {
constexpr double kLow[4] = {0.228, 0.312, 0.552, 0.880}, kHigh[4] = {0.616, 0.840, 1.100, 1.100};
constexpr double kAttLow[4] = {0.192, 0.256, 0.448, 0.712}, kAttHigh[4] = {0.500, 0.676, 0.896, 0.896};
bool highPhase(int hue, int phase) { return mod12(hue + phase) < 6; }
}  // namespace

double signalVoltage(int code, int phase) {
  int hue = code & 15, level = (code >> 4) & 3, emphasis = code >> 6;
  if (hue >= 14) return 0.312;
  bool attenuated = ((emphasis & 1) != 0 && highPhase(12, phase)) || ((emphasis & 2) != 0 && highPhase(4, phase)) ||
                    ((emphasis & 4) != 0 && highPhase(8, phase));
  bool isHigh = hue == 0 || (hue != 13 && highPhase(hue, phase));
  return (isHigh ? (attenuated ? kAttHigh : kHigh) : (attenuated ? kAttLow : kLow))[level];
}

const std::vector<float>& voltageLUT() {
  static const std::vector<float> lut = [] {
    std::vector<float> l(512 * 12);
    for (int phase = 0; phase < 12; ++phase)
      for (int code = 0; code < 512; ++code) l[size_t(phase * 512 + code)] = float(signalVoltage(code, phase));
    return l;
  }();
  return lut;
}

std::array<int32_t, 240> rowPhases(int lineStart0, int initialPhase) {
  std::array<int32_t, 240> out{};
  for (int row = 0; row < 240; ++row) out[size_t(row)] = mod12(mod12(lineStart0 + row * kLineSamples) - kActiveStart + initialPhase);
  return out;
}

std::array<int32_t, 240> rowPhasesForBurst(uint32_t b) { return rowPhases(kActiveStart + 4 * int(b % 3)); }
}  // namespace composite

// ------------------------------------------------------------------ rf
namespace rf {
double sampleRateHz() { return composite::kSampleClockNumerator / composite::kSampleClockDenominator; }

namespace {
Filter designIF() {
  const double fs = sampleRateHz();
  const int delay = (kTaps - 1) / 2;
  std::vector<double> real(kTaps, 0.0), imag(kTaps, 0.0);
  const double tau = 2 * kPi;
  for (int b = 0; b < kQuadratureBins; ++b) {
    double frequency = fs * ((double(b) + 0.5) / double(kQuadratureBins) - 0.5), af = std::fabs(frequency);
    if (af >= kCutoffHz) continue;
    double edge = af <= kCutoffHz - kTransitionHz ? 1 : 0.5 * (1 + std::cos(kPi * (af - kCutoffHz + kTransitionHz) / kTransitionHz));
    double nyquist = frequency <= -kVestigeHz ? 0 : frequency >= kVestigeHz ? 1 : 0.5 + 0.5 * std::sin(kPi * frequency / (2 * kVestigeHz));
    double h = nyquist * edge / double(kQuadratureBins);
    for (int j = 0; j < kTaps; ++j) {
      double angle = tau * frequency * double(j - delay) / fs;
      real[size_t(j)] += h * std::cos(angle);
      imag[size_t(j)] += h * std::sin(angle);
    }
  }
  double gr = 0.0, gi = 0.0;
  for (int j = 0; j < kTaps; ++j) {
    double w = 0.42 - 0.5 * std::cos(tau * double(j) / double(kTaps - 1)) + 0.08 * std::cos(2 * tau * double(j) / double(kTaps - 1));
    real[size_t(j)] *= w;
    imag[size_t(j)] *= w;
    gr += real[size_t(j)];
    gi += imag[size_t(j)];
  }
  double norm = gr * gr + gi * gi;
  for (int j = 0; j < kTaps; ++j) {
    double r = real[size_t(j)], i = imag[size_t(j)];
    real[size_t(j)] = (r * gr + i * gi) / norm;
    imag[size_t(j)] = (i * gr - r * gi) / norm;
  }
  Filter f;
  f.real = std::move(real);
  f.imag = std::move(imag);
  f.delaySamples = delay;
  return f;
}
}  // namespace

const Filter& filter() {
  static const Filter f = designIF();
  return f;
}

namespace {
// Forward FFT (double) of the zero-padded FIR (real, imag), float32 (re, im) pairs.
std::vector<float> spectrumOf(const std::vector<double>& real, const std::vector<double>& imag) {
  const int n = kFFTSize;
  std::vector<double> r(static_cast<size_t>(n), 0.0), q(static_cast<size_t>(n), 0.0);
  for (size_t i = 0; i < real.size(); ++i) { r[i] = real[i]; q[i] = imag[i]; }
  int j = 0;
  for (int i = 1; i < n; ++i) {
    int bit = n >> 1;
    while (j & bit) { j ^= bit; bit >>= 1; }
    j ^= bit;
    if (i < j) { std::swap(r[size_t(i)], r[size_t(j)]); std::swap(q[size_t(i)], q[size_t(j)]); }
  }
  for (int w = 2; w <= n; w *= 2) {
    for (int a = 0; a < n; a += w) {
      for (int k = 0; k < w / 2; ++k) {
        double c = std::cos(-2 * kPi * double(k) / double(w)), s = std::sin(-2 * kPi * double(k) / double(w));
        size_t b = size_t(a + k + w / 2), kk = size_t(a + k);
        double tr = r[b] * c - q[b] * s, ti = r[b] * s + q[b] * c;
        r[b] = r[kk] - tr;
        q[b] = q[kk] - ti;
        r[kk] += tr;
        q[kk] += ti;
      }
    }
  }
  std::vector<float> out(static_cast<size_t>(n) * 2);
  for (int i = 0; i < n; ++i) { out[size_t(i) * 2] = float(r[size_t(i)]); out[size_t(i) * 2 + 1] = float(q[size_t(i)]); }
  return out;
}
}  // namespace

const std::vector<float>& kernelSpectrum() {
  static const std::vector<float> spec = spectrumOf(filter().real, filter().imag);
  return spec;
}

const std::vector<float>& kernelSpectrumReal() {
  static const std::vector<float> spec = spectrumOf(filter().real, std::vector<double>(filter().real.size(), 0.0));
  return spec;
}

const std::vector<float>& twiddles() {
  static const std::vector<float> tw = [] {
    std::vector<float> t(static_cast<size_t>(kFFTSize) * 2);
    for (int i = 0; i < kFFTSize; ++i) {
      t[size_t(i) * 2] = float(std::cos(2 * kPi * double(i) / double(kFFTSize)));
      t[size_t(i) * 2 + 1] = float(std::sin(2 * kPi * double(i) / double(kFFTSize)));
    }
    return t;
  }();
  return tw;
}

// M3-NOISE (assumed): kTB + NF in a 4.2 MHz bandwidth, 75 ohm.
double carrierToNoiseDb(double inputDbuv) {
  return inputDbuv - (-174 + 10 * std::log10(kNoiseBandwidthHz) + kNoiseFigureDb + 108.75);
}
double noiseSigma(double cnrDb, double signalLevel) {
  double sum = 0;
  for (double v : filter().real) sum += v * v;
  double gain = std::sqrt(sum);
  return signalLevel * std::pow(10, -cnrDb / 20) / gain;
}
uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
int32_t noiseKey(uint32_t seed, uint64_t frame) { return int32_t(hash32(seed ^ hash32(uint32_t(frame)))); }
}  // namespace rf

// ------------------------------------------------------------------ phosphor
namespace phosphor {
namespace {
struct Power { double a, alpha, beta; };
struct Channel {
  std::vector<std::pair<double, double>> exponentials;
  bool hasPower;
  Power power;
};
// M4A-DECAY: Kuhn 2002 eqs (8)-(10) fitted to one VGA monitor (inferred).
const std::array<Channel, 3>& channels() {
  static const std::array<Channel, 3> c = {
      Channel{{{4, 360}, {1.75, 1.6e3}, {2, 8e3}, {2.25, 25e3}, {15, 700e3}, {29, 7e6}}, false, {0, 0, 0}},
      Channel{{{37, 150e3}, {100, 700e3}, {90, 5e6}}, true, {210e-6, 5.5e-6, 1.1}},
      Channel{{{75, 100e3}, {1000, 1.1e6}, {1100, 4e6}}, true, {190e-6, 5e-6, 1.11}},
  };
  return c;
}
double rawEnergy(const Channel& ch, double t) {
  if (t <= 0) return 0;
  double sum = 0.0;
  for (const auto& [amplitude, frequency] : ch.exponentials) {
    double tau = 1 / (2 * kPi * frequency);
    sum += amplitude * tau * -std::expm1(-t / tau);
  }
  if (ch.hasPower) {
    const Power& p = ch.power;
    sum += p.a / (p.beta - 1) * (std::pow(p.alpha, 1 - p.beta) - std::pow(t + p.alpha, 1 - p.beta));
  }
  return sum;
}
double cumulativeEnergy(int c, double t, double cutoff) {
  const Channel& ch = channels()[size_t(c)];
  return std::min(t, cutoff) <= 0 ? 0 : rawEnergy(ch, std::min(t, cutoff)) / rawEnergy(ch, cutoff);
}
}  // namespace

std::vector<double> frameFractions(int c, double framePeriod, double cutoff) {
  int count = int(std::ceil(cutoff / framePeriod));
  std::vector<double> out(static_cast<size_t>(count));
  for (int m = 0; m < count; ++m)
    out[size_t(m)] = cumulativeEnergy(c, double(m + 1) * framePeriod, cutoff) - cumulativeEnergy(c, double(m) * framePeriod, cutoff);
  return out;
}
const std::array<std::vector<double>, 3>& fractions() {
  static const std::array<std::vector<double>, 3> f = {frameFractions(0), frameFractions(1), frameFractions(2)};
  return f;
}
int depth() { return int(fractions()[0].size()); }
}  // namespace phosphor

// ------------------------------------------------------------------ tube
namespace tube {
namespace {
struct Panel { double center, length; int periodIndex; };

int imod(int x, int p) { return ((x % p) + p) % p; }

std::vector<Panel> exposedPanels(double low, double high, double period, double center, double openWidth) {
  std::vector<Panel> result;
  int k = int(std::floor((low - center - openWidth / 2) / period));
  int kEnd = int(std::ceil((high - center + openWidth / 2) / period));
  while (k <= kEnd) {
    double left = std::max(low, center + double(k) * period - openWidth / 2);
    double right = std::min(high, center + double(k) * period + openWidth / 2);
    if (right > left) result.push_back(Panel{(left + right) / 2, right - left, k});
    k += 1;
  }
  return result;
}

using WeightList = std::vector<std::pair<int, double>>;

WeightList weights(double position, int size, double sigma, bool normalize) {
  WeightList result;
  double sum = 0.0;
  double radius = kCutoffSigma * sigma;
  int lo = std::max(0, int(std::ceil(position - radius))), hi = std::min(size - 1, int(std::floor(position + radius)));
  if (lo <= hi) {
    for (int i = lo; i <= hi; ++i) {
      double d = (position - double(i)) / sigma;
      double w = std::exp(-0.5 * (d * d)) / (std::sqrt(2 * kPi) * sigma);
      result.push_back({i, w});
      sum += w;
    }
  }
  if (normalize)
    for (auto& e : result) e.second /= sum;
  return result;
}

/// Continuous Gaussian detector integrated over exposed slot material (M1-PRESENTATION).
template <typename Visit>
void detectorQuadrature(int outputCount, double length, int samples, bool horizontal, Visit&& visit) {
  double pixel = length / double(outputCount);
  double sigma = sigmaOutputPixels() * pixel, radius = kReconstructionCutoffSigma * sigma;
  double step = pixel / double(samples), norm = std::sqrt(2 * kPi) * sigma * kTruncatedGaussianMass;
  for (int p = 0; p < outputCount; ++p) {
    double center = (double(p) + 0.5) * pixel, low = std::max(0.0, center - radius), high = std::min(length, center + radius);
    for (int branch = 0; branch < (horizontal ? 3 : 2); ++branch) {
      double period = horizontal ? kTriadPitchM : kVerticalPeriodM;
      double apertureCenter = horizontal ? (double(branch) + 0.5) / 3 * period : (0.5 - double(branch) * kStaggerFraction) * period;
      double apertureWidth = period * (horizontal ? kWidthFraction : kVerticalFill);
      for (const Panel& exposed : exposedPanels(low, high, period, apertureCenter, apertureWidth)) {
        int count = int(std::ceil(exposed.length / step));
        double delta = exposed.length / double(count);
        int target = horizontal ? branch * 2 + imod(exposed.periodIndex, 2) : branch;
        for (int q = 0; q < count; ++q) {
          double coordinate = exposed.center - exposed.length / 2 + (double(q) + 0.5) * delta;
          double d = (coordinate - center) / sigma;
          visit(p, target, coordinate, std::exp(-0.5 * (d * d)) * delta / norm);
        }
      }
    }
  }
}

/// detectorMaps(): per output pixel and branch, (input index, weight) in first-insertion order
/// (JS Map semantics, which fixes the GPU summation order).
std::vector<std::vector<WeightList>> detectorMaps(int outputCount, int inputCount, double length, int samples, bool horizontal,
                                                  double sigmaX, double sigmaY) {
  int branches = horizontal ? 6 : 2;
  std::vector<std::vector<WeightList>> maps(static_cast<size_t>(outputCount), std::vector<WeightList>(static_cast<size_t>(branches)));
  std::vector<int> slot(static_cast<size_t>(inputCount), -1);  // index -> position in the current list
  int lastP = -1, lastT = -1;
  detectorQuadrature(outputCount, length, samples, horizontal, [&](int p, int target, double coordinate, double detector) {
    WeightList& list = maps[size_t(p)][size_t(target)];
    if (lastP != p || lastT != target) {
      // Rebuild the lookup for this (p, target) list (same semantics as the Swift full reset).
      if (lastP >= 0)
        for (const auto& e : maps[size_t(lastP)][size_t(lastT)]) slot[size_t(e.first)] = -1;
      for (size_t k = 0; k < list.size(); ++k) slot[size_t(list[k].first)] = int(k);
      lastP = p;
      lastT = target;
    }
    for (const auto& [i, beam] : weights(coordinate / length * double(inputCount) - 0.5, inputCount, horizontal ? sigmaX : sigmaY, horizontal)) {
      if (slot[size_t(i)] >= 0) list[size_t(slot[size_t(i)])].second += beam * detector;
      else {
        slot[size_t(i)] = int(list.size());
        list.push_back({i, beam * detector});
      }
    }
  });
  return maps;
}

double growthSigma(double u, double mass, double growth) {
  double disc = growth * kGrowthDiscPitches;
  return mass * std::sqrt(kSigmaYPitches * kSigmaYPitches + disc * disc * u / 16);
}

struct GrowthTable {
  int first = 0, count = 0;
  std::vector<double> values;
};

/// growthTables(): K-level vertical tables over a common row support (M4A-SPOT-LAW).
std::vector<std::array<GrowthTable, 2>> growthTables(int oh, int height, int samples, double mass, double growth, int levels) {
  double maxSigma = growthSigma(1, mass, growth);
  std::vector<std::array<std::vector<std::pair<double, double>>, 2>> points(static_cast<size_t>(oh));
  detectorQuadrature(oh, kSurfaceHeightM, samples, false, [&](int p, int target, double coordinate, double detector) {
    points[size_t(p)][size_t(target)].push_back({coordinate / kSurfaceHeightM * double(height) - 0.5, detector});
  });
  std::vector<double> sigmas(static_cast<size_t>(levels));
  for (int k = 0; k < levels; ++k) sigmas[size_t(k)] = growthSigma(levels == 1 ? 0 : double(k) / double(levels - 1), mass, growth);
  std::vector<std::array<GrowthTable, 2>> out(static_cast<size_t>(oh));
  for (int p = 0; p < oh; ++p) {
    for (int t = 0; t < 2; ++t) {
      const auto& pts = points[size_t(p)][size_t(t)];
      if (pts.empty()) continue;
      double mn = pts[0].first, mx = pts[0].first;
      for (const auto& e : pts) { mn = std::min(mn, e.first); mx = std::max(mx, e.first); }
      int lo = std::max(0, int(std::ceil(mn - kCutoffSigma * maxSigma)));
      int hi = std::min(height - 1, int(std::floor(mx + kCutoffSigma * maxSigma)));
      int count = std::max(0, hi - lo + 1);
      std::vector<double> values(static_cast<size_t>(levels) * size_t(count), 0.0);
      for (int k = 0; k < levels; ++k)
        for (const auto& [position, detector] : pts)
          for (const auto& [iy, w] : weights(position, height, sigmas[size_t(k)], false))
            if (iy >= lo && iy <= hi) values[size_t(k * count + iy - lo)] += w * detector;
      out[size_t(p)][size_t(t)] = GrowthTable{lo, count, std::move(values)};
    }
  }
  return out;
}

void pack(const std::vector<std::vector<WeightList>>& maps, int* countOut, std::vector<float>* data) {
  size_t branches = maps[0].size();
  size_t count = 1;
  for (const auto& m : maps)
    for (const auto& l : m) count = std::max(count, l.size());
  data->assign(count * maps.size() * branches * 2, 0.0f);
  for (size_t p = 0; p < maps.size(); ++p)
    for (size_t b = 0; b < branches; ++b)
      for (size_t k = 0; k < maps[p][b].size(); ++k) {
        size_t i = ((p * branches + b) * count + k) * 2;
        (*data)[i] = float(maps[p][b][k].first);
        (*data)[i + 1] = float(maps[p][b][k].second);
      }
  *countOut = int(count);
}
}  // namespace

double sigmaOutputPixels() { return std::sqrt(2 * std::log(100.0)) / kPi; }

double extraSigmaSamples(double u, double growth, int width, int scanLines) {
  double pitchM = kSurfaceHeightM / double(scanLines), sampleM = kSurfaceWidthM / double(width);
  return growth * kGrowthDiscPitches * pitchM * std::sqrt(std::max(0.0, u)) / 4 / sampleM;
}

Plan makePlan(int lines, int ow, int oh, double ambientLux, double beamGrowth, int width, int samples, int beamReferenceWidth,
              int beamReferenceHeight) {
  Plan plan;
  int height = lines;
  double sigmaX = kSigmaXPitches * double(width) / double(beamReferenceWidth);
  double mass = double(height) / double(beamReferenceHeight), sigmaY = kSigmaYPitches * mass;
  auto xs = detectorMaps(ow, width, kSurfaceWidthM, samples, true, sigmaX, sigmaY);
  auto ys = detectorMaps(oh, height, kSurfaceHeightM, samples, false, sigmaX, sigmaY);
  pack(xs, &plan.xCount, &plan.xmap);
  pack(ys, &plan.yCount, &plan.ymap);
  plan.colsum.assign(static_cast<size_t>(ow) * 6, 0.0f);
  plan.growth = beamGrowth > 0;
  int levels = kGrowthLevels;
  plan.growthLevels = levels;
  if (plan.growth) {
    auto tables = growthTables(oh, height, samples, mass, beamGrowth, levels);
    int gCount = 1;
    for (const auto& pair : tables)
      for (const auto& t : pair) gCount = std::max(gCount, t.count);
    plan.gCount = gCount;
    plan.gmap.assign(static_cast<size_t>(levels) * size_t(gCount) * size_t(oh) * 2 * 2, 0.0f);
    for (int y = 0; y < oh; ++y)
      for (int parity = 0; parity < 2; ++parity) {
        const GrowthTable& t = tables[size_t(y)][size_t(parity)];
        for (int k = 0; k < levels; ++k)
          for (int j = 0; j < gCount; ++j) {
            size_t i = (static_cast<size_t>(y * 2 + parity) * size_t(levels) * size_t(gCount) + size_t(k * gCount + j)) * 2;
            if (j < t.count) {
              plan.gmap[i] = float(t.first + j);
              plan.gmap[i + 1] = float(t.values[size_t(k * t.count + j)]);
            } else {
              plan.gmap[i] = -1;
              plan.gmap[i + 1] = 0;
            }
          }
      }
    for (int x = 0; x < ow; ++x)
      for (int b = 0; b < 6; ++b) {
        double s = 0;
        for (const auto& e : xs[size_t(x)][size_t(b)]) s += e.second;
        plan.colsum[size_t(b * ow + x)] = float(s);
      }
  }
  const int counts[2] = {ow, oh};
  for (int axis = 0; axis < 2; ++axis) {
    double sigma = kScatterSigmaM / ((axis == 1 ? kSurfaceHeightM : kSurfaceWidthM) / double(counts[axis]));
    int radius = int(std::ceil(4 * sigma));
    std::vector<double> w(static_cast<size_t>(radius) * 2 + 1, 0.0);
    double total = 0.0;
    for (int d = -radius; d <= radius; ++d) {
      double x = double(d) / sigma;
      w[size_t(d + radius)] = std::exp(-0.5 * (x * x));
      total += w[size_t(d + radius)];
    }
    plan.radius[axis] = radius;
    plan.kernel[axis].resize(w.size());
    for (size_t i = 0; i < w.size(); ++i) plan.kernel[axis][i] = float(w[i] / total);
  }
  // renderRawTube(powered: false): smooth ambient reflection at final pixel centres.
  plan.ambient.assign(static_cast<size_t>(ow) * size_t(oh), 0.0f);
  double lightNorm = std::sqrt(kLightX * kLightX + kLightY * kLightY + kLightZ * kLightZ);
  for (int y = 0; y < oh; ++y)
    for (int x = 0; x < ow; ++x) {
      double nx = ((double(x) + 0.5) / double(ow) * 2 - 1) * kNormalSlopeX;
      double ny = ((double(y) + 0.5) / double(oh) * 2 - 1) * kNormalSlopeY;
      double cosine = std::max(0.0, (nx * kLightX + ny * kLightY + kLightZ) / (std::sqrt(nx * nx + ny * ny + 1) * lightNorm));
      plan.ambient[size_t(y * ow + x)] =
          float(kAlbedo * ambientLux / (kPi * kReferenceWhiteCdM2) * (kDiffuseFraction + (1 - kDiffuseFraction) * cosine));
    }
  plan.width = width;
  plan.height = height;
  plan.outputWidth = ow;
  plan.outputHeight = oh;
  plan.gain = kEmissionGain / (kWidthFraction * kVerticalFill) * mass;
  plan.scatterFraction = kScatterFraction;
  return plan;
}
namespace {
// Least-squares cubic through (l / (n - 1), y[l]) (CRTTube.cubicFit).
std::array<float, 4> cubicFit(const std::vector<double>& y) {
  const size_t n = y.size();
  if (n < 2) return {float(n ? y[0] : 0.0), 0, 0, 0};
  double a[4][5] = {};
  for (size_t i = 0; i < n; ++i) {
    double u = double(i) / double(n - 1);
    double p[4] = {1, u, u * u, u * u * u};
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) a[r][c] += p[r] * p[c];
      a[r][4] += p[r] * y[i];
    }
  }
  for (int c = 0; c < 4; ++c) {
    int piv = c;
    for (int r = c; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
    for (int k = 0; k < 5; ++k) std::swap(a[c][k], a[piv][k]);
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      double f = a[r][c] / a[c][c];
      for (int k = c; k < 5; ++k) a[r][k] -= f * a[c][k];
    }
  }
  return {float(a[0][4] / a[0][0]), float(a[1][4] / a[1][1]), float(a[2][4] / a[2][2]), float(a[3][4] / a[3][3])};
}
}  // namespace

FastPlan fastPlan(const Plan& p) {
  FastPlan f;
  const int ow = p.outputWidth, oh = p.outputHeight, n = p.xCount;
  f.xmap = p.xmap;
  auto at = [&](int x, int b, int k) { return size_t(((x * 6 + b) * n + k) * 2); };
  for (int tile = 0; tile < (ow + 63) / 64; ++tile) {
    const int x0 = tile * 64, x1 = std::min(ow, tile * 64 + 64);
    int lo = INT32_MAX, hi = INT32_MIN;
    for (int x = x0; x < x1; ++x)
      for (int b = 0; b < 6; ++b)
        for (int k = 0; k < n; ++k)
          if (f.xmap[at(x, b, k) + 1] != 0) { lo = std::min(lo, int(f.xmap[at(x, b, k)])); hi = std::max(hi, int(f.xmap[at(x, b, k)])); }
    if (lo > hi) lo = hi = 0;
    for (int x = x0; x < x1; ++x)
      for (int b = 0; b < 6; ++b)
        for (int k = 0; k < n; ++k)
          if (f.xmap[at(x, b, k) + 1] == 0) f.xmap[at(x, b, k)] = float(lo);
    f.tiles.push_back(lo);
    f.tiles.push_back(hi);
    f.maxSpan = std::max(f.maxSpan, hi - lo + 1);
  }
  if (p.growth) {
    f.taps = p.gCount;
    const int L = p.growthLevels, rowWidth = L * p.gCount;
    for (int row = 0; row < oh * 2; ++row) {
      const size_t base = size_t(row) * size_t(rowWidth);
      int count = 0;
      while (count < p.gCount && p.gmap[(base + size_t(count)) * 2] >= 0) ++count;
      f.vrows.push_back(count > 0 ? int32_t(p.gmap[base * 2]) : 0);
      f.vrows.push_back(count);
      for (int k = 0; k < p.gCount; ++k) {
        std::array<float, 4> c{0, 0, 0, 0};
        if (k < count) {
          std::vector<double> y(static_cast<size_t>(L));
          for (int l = 0; l < L; ++l) y[size_t(l)] = double(p.gmap[(base + size_t(l * p.gCount + k)) * 2 + 1]);
          c = cubicFit(y);
        }
        f.vcoef.insert(f.vcoef.end(), c.begin(), c.end());
      }
    }
  } else {
    std::vector<std::vector<float>> rows;
    std::vector<int> firsts;
    for (int row = 0; row < oh * 2; ++row) {
      int lo = INT32_MAX, hi = INT32_MIN;
      for (int k = 0; k < p.yCount; ++k) {
        size_t i = size_t((row * p.yCount + k) * 2);
        if (p.ymap[i + 1] != 0) { lo = std::min(lo, int(p.ymap[i])); hi = std::max(hi, int(p.ymap[i])); }
      }
      if (lo > hi) { rows.emplace_back(); firsts.push_back(0); continue; }
      std::vector<float> w(size_t(hi - lo + 1), 0.0f);
      for (int k = 0; k < p.yCount; ++k) {
        size_t i = size_t((row * p.yCount + k) * 2);
        if (p.ymap[i + 1] != 0) w[size_t(int(p.ymap[i]) - lo)] += p.ymap[i + 1];
      }
      rows.push_back(std::move(w));
      firsts.push_back(lo);
    }
    f.taps = 1;
    for (const auto& w : rows) f.taps = std::max(f.taps, int(w.size()));
    for (size_t row = 0; row < rows.size(); ++row) {
      f.vrows.push_back(firsts[row]);
      f.vrows.push_back(int32_t(rows[row].size()));
      for (int k = 0; k < f.taps; ++k) {
        f.vcoef.push_back(size_t(k) < rows[row].size() ? rows[row][size_t(k)] : 0.0f);
        f.vcoef.insert(f.vcoef.end(), {0.0f, 0.0f, 0.0f});
      }
    }
  }
  f.colinv.assign(size_t(2 * ow) * 4, 0.0f);
  if (p.growth)
    for (int parity = 0; parity < 2; ++parity)
      for (int x = 0; x < ow; ++x)
        for (int c = 0; c < 3; ++c) {
          float cs = p.colsum[size_t((c * 2 + parity) * ow + x)];
          f.colinv[size_t(parity * ow + x) * 4 + size_t(c)] = cs > 0 ? 1 / cs : 0;
        }
  return f;
}

DriveScatter driveScatter(const Plan& p) {
  DriveScatter d;
  const double rows = double(p.height), mass = rows / 240;
  const double sx = kScatterSigmaM / (kSurfaceWidthM / 512), sy = kScatterSigmaM / (kSurfaceHeightM / rows);
  const double bx = kSigmaXPitches * 512 / 256, by = kSigmaYPitches * mass;
  const double dx = sigmaOutputPixels() * 512 / double(p.outputWidth), dy = sigmaOutputPixels() * rows / double(p.outputHeight);
  const double sigX = std::sqrt(sx * sx + bx * bx + dx * dx - 1.0 / 6), sigY = std::sqrt(sy * sy + by * by + dy * dy - 1.0 / 6);
  d.rx = int(std::ceil(4 * sigX));
  d.ry = int(std::ceil(4 * sigY));
  auto kernel = [](double sigma, int r) {
    std::vector<double> w;
    double t = 0;
    for (int i = -r; i <= r; ++i) { double x = double(i) / sigma; w.push_back(std::exp(-0.5 * x * x)); t += w.back(); }
    std::vector<float> out;
    for (double v : w) out.push_back(float(v / t));
    return out;
  };
  d.wx = kernel(sigX, d.rx);
  d.wy = kernel(sigY, d.ry);
  // Flat field u: h = u * colsum, emission = gain * u * sum_parity colsum(x) * V(y) - separable.
  const double u = 0.5;
  const int ow = p.outputWidth, oh = p.outputHeight;
  for (int c = 0; c < 3; ++c) {
    double total = 0;
    for (int parity = 0; parity < 2; ++parity) {
      double meanX = 0;
      for (int x = 0; x < ow; ++x) {
        if (p.growth) meanX += double(p.colsum[size_t((c * 2 + parity) * ow + x)]);
        else for (int k = 0; k < p.xCount; ++k) meanX += double(p.xmap[size_t(((x * 6 + c * 2 + parity) * p.xCount + k) * 2 + 1)]);
      }
      meanX /= double(ow);
      double meanY = 0;
      for (int y = 0; y < oh; ++y) {
        const int row = y * 2 + parity;
        if (p.growth) {
          const int L = p.growthLevels, G = p.gCount, kk = std::min(L - 2, int(u * double(L - 1)));
          const double t = u * double(L - 1), f = t - double(kk);
          for (int k = 0; k < G; ++k) {
            size_t base = size_t(row) * size_t(L * G);
            if (p.gmap[(base + size_t(k)) * 2] < 0) continue;
            meanY += double(p.gmap[(base + size_t(kk * G + k)) * 2 + 1]) * (1 - f) + double(p.gmap[(base + size_t((kk + 1) * G + k)) * 2 + 1]) * f;
          }
        } else {
          for (int k = 0; k < p.yCount; ++k) meanY += double(p.ymap[size_t((row * p.yCount + k) * 2 + 1)]);
        }
      }
      total += meanX * meanY / double(oh);
    }
    d.kappa[c] = float(total * p.gain * p.scatterFraction);
  }
  return d;
}

}  // namespace tube

}  // namespace rnl::crt
