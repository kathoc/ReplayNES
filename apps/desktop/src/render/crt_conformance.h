// The CRT conformance test shared by the GPU ports (header-only; one backend per graphics API):
//   Vulkan       apps/linux/src/render/crt_test.cpp      (replaynes-crt-test, ctest crt_conformance)
//   Direct3D 11  apps/windows/src/crt_test_d3d11.cpp     (test_crt_d3d11, ctest crt_conformance_d3d11)
// against nesterm's CPU reference models (tests/fixtures/crt/reference.json,
// tools/crt-reference/generate-fixtures.mjs) with the same inputs and tolerances as the macOS
// CRTConformance / CRTTests, plus determinism, the fast kernels == direct port bit identity, the
// setup model, and the fast display path (CrtQuality::fast) against the reference path (PSNR of the
// displayed picture on moving input, a still after a seek, determinism). A Backend provides:
//   using Renderer = ...;  (flags noiseEnabled, disableSpotH, useFastFFT, useFastScatter)
//   std::unique_ptr<Renderer> make(const CrtSettings&, int ow, int oh, CrtQuality);  // plan built synchronously
//   bool run(Renderer&, const CrtInput&, uint64_t ordinal);              // encode + wait
//   std::vector<float> read(Renderer&, CrtStage);                        // stage buffer, RGBA floats
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "render/crt_model.h"
#include "render/crt_settings.h"
#include "render/crt_types.h"

namespace rnl::crt_conformance {

// ------------------------------------------------------------------ minimal JSON
struct Json {
  enum class T { null, num, str, arr, obj, boolean } t = T::null;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::map<std::string, Json> obj;
  const Json* get(const std::string& k) const {
    auto it = obj.find(k);
    return it == obj.end() ? nullptr : &it->second;
  }
};
struct Parser {
  const std::string& s;
  size_t i = 0;
  void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
  Json parse() {
    ws();
    Json j;
    if (i >= s.size()) return j;
    char c = s[i];
    if (c == '{') {
      j.t = Json::T::obj;
      ++i;
      ws();
      if (s[i] == '}') { ++i; return j; }
      for (;;) {
        ws();
        std::string k = parse().str;
        ws();
        ++i;  // ':'
        j.obj[k] = parse();
        ws();
        if (s[i] == ',') { ++i; continue; }
        ++i;  // '}'
        return j;
      }
    }
    if (c == '[') {
      j.t = Json::T::arr;
      ++i;
      ws();
      if (s[i] == ']') { ++i; return j; }
      for (;;) {
        j.arr.push_back(parse());
        ws();
        if (s[i] == ',') { ++i; continue; }
        ++i;
        return j;
      }
    }
    if (c == '"') {
      j.t = Json::T::str;
      ++i;
      while (s[i] != '"') {
        if (s[i] == '\\') ++i;
        j.str += s[i++];
      }
      ++i;
      return j;
    }
    if (s.compare(i, 4, "true") == 0) { i += 4; j.t = Json::T::boolean; j.num = 1; return j; }
    if (s.compare(i, 5, "false") == 0) { i += 5; j.t = Json::T::boolean; return j; }
    if (s.compare(i, 4, "null") == 0) { i += 4; return j; }
    size_t end = i;
    while (end < s.size() && (std::isdigit((unsigned char)s[end]) || s[end] == '-' || s[end] == '+' || s[end] == '.' || s[end] == 'e' || s[end] == 'E')) ++end;
    j.t = Json::T::num;
    j.num = std::strtod(s.substr(i, end - i).c_str(), nullptr);
    i = end;
    return j;
  }
};

inline std::vector<float> base64Floats(const std::string& in) {
  static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<uint8_t> bytes;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=') break;
    size_t v = tbl.find(c);
    if (v == std::string::npos) continue;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(uint8_t((acc >> bits) & 0xFF));
    }
  }
  std::vector<float> f(bytes.size() / 4);
  std::memcpy(f.data(), bytes.data(), f.size() * 4);
  return f;
}

struct Case {
  int width = 0, height = 0;
  std::vector<int> rows;
  std::vector<float> data;
  const Json* meta = nullptr;
};

// ------------------------------------------------------------------ inputs (mirror generate-fixtures.mjs)
inline std::vector<uint16_t> testCodes() {
  const uint16_t bars[8] = {0x30, 0x28, 0x2C, 0x2A, 0x24, 0x16, 0x12, 0x0F};
  std::vector<uint16_t> c(256 * 240);
  for (int y = 0; y < 240; ++y)
    for (int x = 0; x < 256; ++x) {
      int v;
      if (y < 100) v = bars[x >> 5];
      else if (y < 150) v = ((x >> 4) & 15) | ((((y - 100) / 13) & 3) << 4);
      else if (y < 200) v = ((x >> 4) & 15) | (2 << 4) | ((((y - 150) / 7) & 7) << 6);
      else v = ((x >> 3) + (y >> 3)) % 2 == 0 ? 0x30 : 0x0F;
      c[size_t(y * 256 + x)] = uint16_t(v);
    }
  return c;
}

/// variant 0: plain; 1: min(1, v + 0.5); 2: 1 - v; 3: v * 0.25 (double arithmetic like JS).
inline std::vector<float> testDrive(int variant = 0) {
  std::vector<float> d(512 * 240 * 4, 0.0f);
  for (int y = 0; y < 240; ++y)
    for (int x = 0; x < 512; ++x) {
      for (int c = 0; c < 3; ++c) {
        double v;
        if (x < 128) v = double((x + 3 * y + 50 * c) % 200) / 199;
        else if (x < 256) v = ((x >> 4) + y / 30 + c) % 3 == 0 ? 1 : 0.05 * double(c);
        else if (x < 384) v = ((x >> 1) + y) % 2 != 0 ? 0.8 : 0;
        else v = double((x * 37 + y * 101 + c * 53) % 1000) / 999;
        switch (variant) {
          case 1: v = std::min(1.0, v + 0.5); break;
          case 2: v = 1 - v; break;
          case 3: v = v * 0.25; break;
          default: break;
        }
        d[size_t(y * 512 + x) * 4 + size_t(c)] = float(v);
      }
      d[size_t(y * 512 + x) * 4 + 3] = 1;
    }
  return d;
}

struct Diff {
  double maxAbs = 0, meanAbs = 0;
  long count = 0;
  std::string str() const {
    char b[96];
    std::snprintf(b, sizeof b, "max %.3g mean %.3g (n=%ld)", maxAbs, meanAbs, count);
    return b;
  }
};

template <class Backend>
class Conformance {
 public:
  using Renderer = typename Backend::Renderer;
  Conformance(Backend& backend, std::map<std::string, Case> cases) : backend_(backend), cases_(std::move(cases)) {}

  /// The conformance checks run the reference path; the fast path is checked against it.
  std::unique_ptr<Renderer> renderer(const CrtSettings& s, int ow = 256, int oh = 192, CrtQuality q = CrtQuality::reference) {
    return backend_.make(s, ow, oh, q);
  }
  bool run(Renderer& r, const CrtInput& in, uint64_t ordinal) { return backend_.run(r, in, ordinal); }
  std::vector<float> read(Renderer& r, CrtStage stage) { return backend_.read(r, stage); }
  Diff compare(const std::string& name, const std::vector<float>& got, int width) {
    Diff d;
    auto it = cases_.find(name);
    if (it == cases_.end() || got.empty()) { d.maxAbs = INFINITY; return d; }
    const Case& c = it->second;
    double sum = 0;
    for (size_t i = 0; i < c.rows.size(); ++i)
      for (int x = 0; x < c.width; ++x)
        for (int ch = 0; ch < 3; ++ch) {
          double ref = c.data[(i * size_t(c.width) + size_t(x)) * 3 + size_t(ch)];
          size_t gi = (size_t(c.rows[i]) * size_t(width) + size_t(x)) * 4 + size_t(ch);
          double v = gi < got.size() ? got[gi] : NAN;
          double e = std::fabs(ref - v);
          d.maxAbs = std::max(d.maxAbs, std::isnan(e) ? INFINITY : e);
          sum += e;
          d.count += 1;
        }
    d.meanAbs = sum / double(std::max(1L, d.count));
    return d;
  }
  static CrtSettings off() {
    CrtSettings s;
    s.lines = 240;
    s.beamGrowth = false;
    s.persistence = false;
    s.supply = false;
    return s;
  }
  CrtInput codes(uint32_t burst) {
    CrtInput in;
    in.kind = CrtInputKind::codes;
    in.codes = codes_.data();
    in.burstPhase = burst;
    return in;
  }
  CrtInput drive(const std::vector<float>& d) {
    CrtInput in;
    in.kind = CrtInputKind::drive;
    in.drive = d.data();
    return in;
  }

  Diff receiver(bool noise) {
    CrtSettings s = off();
    if (noise) s.antennaDbuv = 30;
    auto r = renderer(s);
    if (!r) return Diff{INFINITY};
    r->noiseEnabled = noise;
    run(*r, codes(1), 7);
    return compare(noise ? "receiverNoise" : "receiver", read(*r, CrtStage::receiver), 512);
  }
  Diff tube(bool growth) {
    CrtSettings s = off();
    s.beamGrowth = growth;
    auto r = renderer(s);
    if (!r) return Diff{INFINITY};
    r->disableSpotH = true;
    auto d = testDrive();
    run(*r, drive(d), 1);
    return compare(growth ? "tubeGrowth" : "tube", read(*r, CrtStage::output), 256);
  }
  Diff raster160() {
    CrtSettings s = off();
    s.lines = 160;
    auto r = renderer(s);
    if (!r) return Diff{INFINITY};
    auto d = testDrive();
    run(*r, drive(d), 1);
    return compare("raster160", read(*r, CrtStage::tubeInput), 512);
  }
  Diff supply() {
    CrtSettings s = off();
    s.supply = true;
    auto r = renderer(s);
    if (!r) return Diff{INFINITY};
    run(*r, drive(black_), 0);  // nesterm's idle start (see black_)
    for (int f = 0; f < 3; ++f) {
      auto d = testDrive(f == 1 ? 1 : 0);
      run(*r, drive(d), uint64_t(f + 1));
    }
    return compare("supply", read(*r, CrtStage::tubeInput), 512);
  }
  Diff spotH() {
    CrtSettings s = off();
    s.beamGrowth = true;
    auto r = renderer(s);
    if (!r) return Diff{INFINITY};
    auto d = testDrive();
    run(*r, drive(d), 1);
    return compare("spotH", read(*r, CrtStage::tubeInput), 512);
  }
  Diff persistence() {
    CrtSettings s = off();
    s.persistence = true;
    auto r = renderer(s);
    if (!r) return Diff{INFINITY};
    run(*r, drive(black_), 9);  // nesterm's empty history (see black_)
    for (int f = 0; f < 3; ++f) {
      auto d = testDrive(f == 0 ? 2 : f == 1 ? 3 : 0);
      run(*r, drive(d), uint64_t(10 + f));
    }
    return compare("persistence", read(*r, CrtStage::output), 256);
  }
  /// A still after a discontinuity (seek / rewind / load: a non-increasing ordinal) vs the same
  /// picture shown in continuous play, all effects on, noise off; `phases` = burst phase per
  /// sequence frame (the still uses the last). Worst of the re-encoded still and a fresh
  /// renderer's first frame: max abs diff and worst per-channel mean difference (the tint).
  struct StillDiff { double maxAbs = INFINITY, meanAbs = INFINITY; };
  StillDiff stillAfterSeek(const std::vector<uint32_t>& phases, CrtQuality q = CrtQuality::reference) {
    auto diff = [](const std::vector<float>& a, const std::vector<float>& b) {
      StillDiff d;
      if (a.empty() || a.size() != b.size()) return d;
      double m = 0, sa[3] = {0, 0, 0}, sb[3] = {0, 0, 0};
      for (size_t i = 0; i + 3 < a.size(); i += 4)
        for (int c = 0; c < 3; ++c) {
          double e = std::fabs(double(a[i + size_t(c)]) - double(b[i + size_t(c)]));
          m = std::max(m, std::isnan(e) ? INFINITY : e);
          sa[c] += a[i + size_t(c)];
          sb[c] += b[i + size_t(c)];
        }
      double n = double(a.size() / 4), worst = 0;
      for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(sa[c] - sb[c]) / n);
      d.maxAbs = m;
      d.meanAbs = worst;
      return d;
    };
    const uint64_t last = phases.size();
    auto r = renderer(CrtSettings(), 256, 192, q);
    auto fresh = renderer(CrtSettings(), 256, 192, q);
    if (!r || !fresh || phases.empty()) return StillDiff{};
    r->noiseEnabled = false;
    fresh->noiseEnabled = false;
    for (size_t i = 0; i < phases.size(); ++i) run(*r, codes(phases[i]), uint64_t(i + 1));
    auto played = read(*r, CrtStage::output);
    run(*r, codes(phases.back()), last);  // seek back to the same frame
    StillDiff a = diff(played, read(*r, CrtStage::output));
    run(*fresh, codes(phases.back()), last);
    StillDiff b = diff(played, read(*fresh, CrtStage::output));
    return StillDiff{std::max(a.maxAbs, b.maxAbs), std::max(a.meanAbs, b.meanAbs)};
  }
  double lastFastPathMaxAbs = 0;
  bool fastPathsMatchDirectPort(int ow = 256, int oh = 192) {
    size_t receiverFloats = 0;
    std::vector<std::vector<float>> outs;
    for (bool fast : {false, true}) {
      auto r = renderer(CrtSettings(), ow, oh);
      if (!r) return false;
      r->useFastFFT = fast;
      r->useFastScatter = fast;
      for (int f = 0; f < 3; ++f) run(*r, codes(uint32_t(f % 3)), uint64_t(f));
      auto a = read(*r, CrtStage::receiver), b = read(*r, CrtStage::output);
      receiverFloats = a.size();
      a.insert(a.end(), b.begin(), b.end());
      outs.push_back(a);
    }
    if (outs[0].size() != outs[1].size() || outs[0].empty()) return false;
    size_t diff = 0, diffReceiver = 0;
    double maxAbs = 0;
    for (size_t i = 0; i < outs[0].size(); ++i) {
      bool d = std::memcmp(&outs[0][i], &outs[1][i], 4) != 0;
      diff += d;
      diffReceiver += d && i < receiverFloats;
      maxAbs = std::max(maxAbs, double(std::fabs(outs[0][i] - outs[1][i])));
    }
    if (diff)
      std::printf("    %zu of %zu floats differ (receiver %zu, tube output %zu), max %.3g\n", diff, outs[0].size(), diffReceiver,
                  diff - diffReceiver, maxAbs);
    lastFastPathMaxAbs = maxAbs;
    return diff == 0;
  }
  bool deterministic(CrtQuality q = CrtQuality::reference) {
    std::vector<std::vector<float>> outs;
    for (int k = 0; k < 2; ++k) {
      auto r = renderer(CrtSettings(), 256, 192, q);
      if (!r) return false;
      for (int f = 0; f < 4; ++f) run(*r, codes(uint32_t(f % 3)), uint64_t(f));
      outs.push_back(read(*r, CrtStage::output));
    }
    return !outs[0].empty() && outs[0].size() == outs[1].size() && std::memcmp(outs[0].data(), outs[1].data(), outs[0].size() * 4) == 0;
  }
  /// The fast path against the reference on the same moving sequence (the fixture codes scrolled 3
  /// pixels and the burst phase advanced per frame): the worst frame's PSNR of the displayed
  /// (sRGB-encoded, clamped) tube picture over the last `checked` frames (CRTConformance.fastVsReference).
  struct Quality { double psnr = INFINITY, maxAbs = 0, meanAbs = 0; };
  Quality fastVsReference(const CrtSettings& s, int ow = 320, int oh = 240, bool rgb = false, int frames = 10, int checked = 4) {
    auto ref = renderer(s, ow, oh, CrtQuality::reference), fast = renderer(s, ow, oh, CrtQuality::fast);
    Quality q;
    if (!ref || !fast) { q.psnr = -INFINITY; return q; }
    auto encode = [](float v) {
      double x = std::min(std::max(double(v), 0.0), 1.0);
      return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
    };
    std::vector<uint16_t> c(256 * 240);
    std::vector<uint32_t> px(256 * 240);
    for (int f = 0; f < frames; ++f) {
      for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 256; ++x) c[size_t(y * 256 + x)] = codes_[size_t(y * 256 + (x + 3 * f) % 256)];
      for (size_t i = 0; i < px.size(); ++i) px[i] = 0xFF000000u | uint32_t(c[i] & 0x3F) * 0x00040201u;
      CrtInput in;
      if (rgb) { in.kind = CrtInputKind::rgb; in.rgb = px.data(); }
      else { in.kind = CrtInputKind::codes; in.codes = c.data(); in.burstPhase = uint32_t(f % 3); }
      run(*ref, in, uint64_t(f + 1));
      run(*fast, in, uint64_t(f + 1));
      if (f < frames - checked) continue;
      auto a = read(*ref, CrtStage::output), b = read(*fast, CrtStage::output);
      const size_t n = size_t(ow) * size_t(oh);
      if (a.size() < n * 4 || b.size() < n * 4) { q.psnr = -INFINITY; return q; }
      double se = 0, sum = 0, mx = 0;
      for (size_t i = 0; i < n; ++i)
        for (int ch = 0; ch < 3; ++ch) {
          double d = std::fabs(encode(a[i * 4 + size_t(ch)]) - encode(b[i * 4 + size_t(ch)]));
          se += d * d;
          sum += d;
          mx = std::max(mx, std::isnan(d) ? INFINITY : d);
        }
      q.psnr = std::min(q.psnr, se > 0 ? 10 * std::log10(double(n * 3) / se) : 200.0);
      q.maxAbs = std::max(q.maxAbs, mx);
      q.meanAbs = std::max(q.meanAbs, sum / double(n * 3));
    }
    return q;
  }
  const Case* find(const std::string& n) const {
    auto it = cases_.find(n);
    return it == cases_.end() ? nullptr : &it->second;
  }

 private:
  Backend& backend_;
  std::map<std::string, Case> cases_;
  std::vector<uint16_t> codes_ = testCodes();
  // ReplayNES starts the temporal state from the first picture held (supply steady state,
  // persistence history filled with it); nesterm's reference starts from an idle tube and an empty
  // history, which is exactly a black frame held before the sequence.
  std::vector<float> black_ = [] {
    std::vector<float> d(512 * 240 * 4, 0.0f);
    for (size_t i = 3; i < d.size(); i += 4) d[i] = 1;
    return d;
  }();
};

struct Checker {
  int failures = 0;
  void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
  }
};

/// Runs every check; prints one line each. Returns the number of failures (-1: fixture missing).
/// `deviceFusesMultiplyAdd`: the device was seen to fuse a multiply and an add marked `precise`
/// (a driver that ignores the no-contraction rule, e.g. Parallels' Direct3D-over-Metal adapter):
/// the restructured kernels can't be bit-identical to the direct port there, so they are checked
/// to agree within 2e-3 (the half-float persistence tolerance) instead.
template <class Backend>
int run(Backend& backend, const std::string& fixture, bool deviceFusesMultiplyAdd = false) {
  Checker checker;
  std::ifstream f(fixture, std::ios::binary);
  if (!f) { std::fprintf(stderr, "missing fixture %s\n", fixture.c_str()); return -1; }
  std::stringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  Parser p{text};
  static Json root;  // the cases keep pointers into it
  root = p.parse();
  std::map<std::string, Case> cases;
  if (const Json* cs = root.get("cases")) {
    for (const auto& [name, c] : cs->obj) {
      Case k;
      if (const Json* v = c.get("width")) k.width = int(v->num);
      if (const Json* v = c.get("height")) k.height = int(v->num);
      if (const Json* v = c.get("rows")) for (const Json& r : v->arr) k.rows.push_back(int(r.num));
      if (const Json* v = c.get("data")) k.data = base64Floats(v->str);
      k.meta = &c;
      cases[name] = std::move(k);
    }
  }
  Conformance<Backend> c(backend, cases);

  // Setup model (CRTTests.testSetupModel).
  {
    double sr = 0, si = 0;
    for (double v : crt::rf::filter().real) sr += v;
    for (double v : crt::rf::filter().imag) si += v;
    checker.check(std::fabs(sr - 1) < 1e-10 && std::fabs(si) < 1e-10, "designIF unity complex carrier gain");
    checker.check(crt::rf::filter().delaySamples == 512 && crt::rf::kBlocks == 214, "IF delay 512, 214 FFT blocks");
    bool sums = true;
    for (int ch = 0; ch < 3; ++ch) {
      double s = 0;
      for (double v : crt::phosphor::fractions()[size_t(ch)]) s += v;
      sums &= std::fabs(s - 1) < 1e-12;
    }
    checker.check(sums && crt::phosphor::depth() == 7, "phosphor frame fractions sum to 1, depth 7");
    auto ph = crt::composite::rowPhasesForBurst(2);
    checker.check(ph[0] == 8 && ph[1] == 0, "burst phase -> row carrier phases");
    int w = 0, h = 0, w2 = 0, h2 = 0;
    crtTubeSize(3000, 2200, 0, 1600, &w, &h);
    crtTubeSize(800, 600, 0, 1600, &w2, &h2);
    checker.check(w == 1600 && h2 == 600, "tube sizes bounded like OUTPUT-RESOLUTION-SPEC");
  }
  if (const Case* nh = c.find("noiseHash")) {
    const uint32_t in[4] = {0, 1, 7, 654719};
    bool ok = nh->meta && nh->meta->get("hash") && nh->meta->get("hash")->arr.size() == 4;
    for (int i = 0; ok && i < 4; ++i) ok = crt::rf::hash32(in[i]) == uint32_t(nh->meta->get("hash")->arr[size_t(i)].num);
    ok = ok && nh->meta->get("key") && crt::rf::noiseKey(1, 7) == int32_t(nh->meta->get("key")->num);
    checker.check(ok, "noise hash32 / noiseKey match the reference");
  } else {
    checker.check(false, "noiseHash fixture");
  }
  // Float32 GPU vs float64 CPU reference; the same bounds as the macOS CRTTests.
  Diff d;
  d = c.receiver(false);  checker.check(d.maxAbs < 1e-4 && d.count > 10000, "receiver (AGC) " + d.str());
  d = c.receiver(true);   checker.check(d.maxAbs < 1e-4, "receiver with antenna noise " + d.str());
  d = c.tube(false);      checker.check(d.maxAbs < 1e-5, "tube, fixed spot " + d.str());
  d = c.tube(true);       checker.check(d.maxAbs < 1e-5, "tube, beam growth " + d.str());
  d = c.raster160();      checker.check(d.maxAbs < 1e-5, "reduced raster 160 lines " + d.str());
  d = c.supply();         checker.check(d.maxAbs < 1e-3, "supply / ABL (3 frames) " + d.str());
  d = c.spotH();          checker.check(d.maxAbs < 1e-5, "horizontal spot " + d.str());
  d = c.persistence();    checker.check(d.maxAbs < 2e-3, "persistence (half-float ring) " + d.str());
  checker.check(c.deterministic(), "same frame sequence is bit-identical");
  {
    // Regression: after a seek / rewind / load a still showed only the first frame's share of each
    // phosphor's light (green/blue ~10% darker: a purple tint) and an idle supply.
    auto st = c.stillAfterSeek(std::vector<uint32_t>(12, 1));
    char b[128];
    std::snprintf(b, sizeof b, "still after a seek == same picture in continuous play (max %.3g, mean %.3g)", st.maxAbs, st.meanAbs);
    checker.check(st.maxAbs < 3e-3, b);
    std::vector<uint32_t> alt;
    for (int i = 0; i < 12; ++i) alt.push_back(i % 2 == 0 ? 2u : 0u);
    st = c.stillAfterSeek(alt);
    std::snprintf(b, sizeof b, "still after a seek: same colour as alternating-phase play (mean %.3g)", st.meanAbs);
    checker.check(st.meanAbs < 2e-3, b);
  }
  auto fastPaths = [&](int ow, int oh, const std::string& what) {
    bool same = c.fastPathsMatchDirectPort(ow, oh);
    if (same || !deviceFusesMultiplyAdd) return checker.check(same, what);
    char b[96];
    std::snprintf(b, sizeof b, " within 2e-3 (max %.3g; the device fuses precise multiply-adds)", c.lastFastPathMaxAbs);
    checker.check(c.lastFastPathMaxAbs < 2e-3, what + b);
  };
  fastPaths(256, 192, "restructured kernels (shared-memory FFT/scatter, register taps) == direct port, 256x192");
  fastPaths(1144, 858, "restructured kernels == direct port, 1144x858 (Deck full screen tube)");
  // Fast display path (CrtQuality::fast) against the reference: observed 60-75 dB; the bound
  // (45 dB, an 8-bit display step is ~48 dB) leaves room for other GPUs' transcendental functions
  // without letting a structural error through.
  auto fastCheck = [&](const CrtSettings& s, int ow, int oh, bool rgb, const char* what) {
    auto q = c.fastVsReference(s, ow, oh, rgb);
    char b[160];
    std::snprintf(b, sizeof b, "fast path vs reference, %s %dx%d: PSNR %.1f dB, max %.3g, mean %.3g", what, ow, oh, q.psnr, q.maxAbs,
                  q.meanAbs);
    checker.check(q.psnr > 45 && q.meanAbs < 2e-3, b);
  };
  fastCheck(CrtSettings(), 320, 240, false, "defaults");
  fastCheck(CrtSettings(), 640, 480, false, "defaults");
  fastCheck(Conformance<Backend>::off(), 320, 240, false, "effects off");
  {
    CrtSettings s;
    s.lines = 160;
    fastCheck(s, 320, 240, false, "160 lines");
  }
  fastCheck(CrtSettings(), 320, 240, true, "RGB input");
  {
    auto st = c.stillAfterSeek(std::vector<uint32_t>(12, 1), CrtQuality::fast);
    char b[128];
    std::snprintf(b, sizeof b, "fast path: still after a seek == continuous play (max %.3g)", st.maxAbs);
    checker.check(st.maxAbs < 3e-3, b);
  }
  checker.check(c.deterministic(CrtQuality::fast), "fast path: same frame sequence is bit-identical");
  std::printf(checker.failures ? "%d FAILED\n" : "all passed\n", checker.failures);
  return checker.failures;
}

}  // namespace rnl::crt_conformance
