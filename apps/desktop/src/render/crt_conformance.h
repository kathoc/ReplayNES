// The CRT conformance test shared by the GPU ports (header-only; one backend per graphics API):
//   Vulkan       apps/linux/src/render/crt_test.cpp      (replaynes-crt-test, ctest crt_conformance)
//   Direct3D 11  apps/windows/src/crt_test_d3d11.cpp     (test_crt_d3d11, ctest crt_conformance_d3d11)
// against nesterm's CPU reference models (tests/fixtures/crt/reference.json,
// tools/crt-reference/generate-fixtures.mjs) with the same inputs and tolerances as the macOS
// CRTConformance / CRTTests, plus determinism, the fast kernels == direct port bit identity and
// the setup model. A Backend provides:
//   using Renderer = ...;  (flags noiseEnabled, disableSpotH, useFastFFT, useFastScatter)
//   std::unique_ptr<Renderer> make(const CrtSettings&, int ow, int oh);  // tube plan built synchronously
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

  std::unique_ptr<Renderer> renderer(const CrtSettings& s, int ow = 256, int oh = 192) { return backend_.make(s, ow, oh); }
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
    for (int f = 0; f < 3; ++f) {
      auto d = testDrive(f == 0 ? 2 : f == 1 ? 3 : 0);
      run(*r, drive(d), uint64_t(10 + f));
    }
    return compare("persistence", read(*r, CrtStage::output), 256);
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
  bool deterministic() {
    std::vector<std::vector<float>> outs;
    for (int k = 0; k < 2; ++k) {
      auto r = renderer(CrtSettings());
      if (!r) return false;
      for (int f = 0; f < 4; ++f) run(*r, codes(uint32_t(f % 3)), uint64_t(f));
      outs.push_back(read(*r, CrtStage::output));
    }
    return !outs[0].empty() && outs[0].size() == outs[1].size() && std::memcmp(outs[0].data(), outs[1].data(), outs[0].size() * 4) == 0;
  }
  const Case* find(const std::string& n) const {
    auto it = cases_.find(n);
    return it == cases_.end() ? nullptr : &it->second;
  }

 private:
  Backend& backend_;
  std::map<std::string, Case> cases_;
  std::vector<uint16_t> codes_ = testCodes();
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
  auto fastPaths = [&](int ow, int oh, const std::string& what) {
    bool same = c.fastPathsMatchDirectPort(ow, oh);
    if (same || !deviceFusesMultiplyAdd) return checker.check(same, what);
    char b[96];
    std::snprintf(b, sizeof b, " within 2e-3 (max %.3g; the device fuses precise multiply-adds)", c.lastFastPathMaxAbs);
    checker.check(c.lastFastPathMaxAbs < 2e-3, what + b);
  };
  fastPaths(256, 192, "restructured kernels (shared-memory FFT/scatter, register taps) == direct port, 256x192");
  fastPaths(1144, 858, "restructured kernels == direct port, 1144x858 (Deck full screen tube)");
  std::printf(checker.failures ? "%d FAILED\n" : "all passed\n", checker.failures);
  return checker.failures;
}

}  // namespace rnl::crt_conformance
