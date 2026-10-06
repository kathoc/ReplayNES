// Headless conformance test of the Vulkan CRT port against nesterm's CPU reference models
// (tests/fixtures/crt/reference.json, tools/crt-reference/generate-fixtures.mjs) with the same
// inputs and tolerances as the macOS CRTConformance / CRTTests, plus determinism, the fast
// kernels == direct port bit identity, the setup model, and a GPU benchmark.
//   replaynes-crt-test [--fixture FILE]                 conformance (exit 0 pass, 1 fail, 77 no device)
//   replaynes-crt-test --bench WxH [--frames N] [--settings default|off|nogrowth] [--rgb]
// REPLAYNES_VK_DEVICE=<name substring> picks the device (e.g. llvmpipe = lavapipe).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "render/crt_model.h"
#include "render/crt_renderer.h"
#include "render/headless_vulkan.h"

using namespace rnl;

namespace {

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

std::vector<float> base64Floats(const std::string& in) {
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
std::vector<uint16_t> testCodes() {
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
std::vector<float> testDrive(int variant = 0) {
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

class Conformance {
 public:
  Conformance(HeadlessVulkan& vk, std::map<std::string, Case> cases) : vk_(vk), cases_(std::move(cases)) {}

  std::unique_ptr<CrtRenderer> renderer(const CrtSettings& s, int ow = 256, int oh = 192) {
    auto r = std::make_unique<CrtRenderer>();
    std::string err;
    if (!r->init(vk_.context(), VK_NULL_HANDLE, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return nullptr;
    }
    r->configure(s, ow, oh, true);
    return r;
  }
  bool run(CrtRenderer& r, const CrtRenderer::Input& in, uint64_t ordinal) {
    bool encoded = false;
    bool ok = r.runSync([&](VkCommandBuffer cmd) { encoded = r.encode(cmd, in, ordinal); });
    return ok && encoded;
  }
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
  CrtRenderer::Input codes(uint32_t burst) {
    CrtRenderer::Input in;
    in.kind = CrtRenderer::InputKind::codes;
    in.codes = codes_.data();
    in.burstPhase = burst;
    return in;
  }
  CrtRenderer::Input drive(const std::vector<float>& d) {
    CrtRenderer::Input in;
    in.kind = CrtRenderer::InputKind::drive;
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
    return compare(noise ? "receiverNoise" : "receiver", r->read(CrtRenderer::Stage::receiver), 512);
  }
  Diff tube(bool growth) {
    CrtSettings s = off();
    s.beamGrowth = growth;
    auto r = renderer(s);
    if (!r) return Diff{INFINITY};
    r->disableSpotH = true;
    auto d = testDrive();
    run(*r, drive(d), 1);
    return compare(growth ? "tubeGrowth" : "tube", r->read(CrtRenderer::Stage::output), 256);
  }
  Diff raster160() {
    CrtSettings s = off();
    s.lines = 160;
    auto r = renderer(s);
    if (!r) return Diff{INFINITY};
    auto d = testDrive();
    run(*r, drive(d), 1);
    return compare("raster160", r->read(CrtRenderer::Stage::tubeInput), 512);
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
    return compare("supply", r->read(CrtRenderer::Stage::tubeInput), 512);
  }
  Diff spotH() {
    CrtSettings s = off();
    s.beamGrowth = true;
    auto r = renderer(s);
    if (!r) return Diff{INFINITY};
    auto d = testDrive();
    run(*r, drive(d), 1);
    return compare("spotH", r->read(CrtRenderer::Stage::tubeInput), 512);
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
    return compare("persistence", r->read(CrtRenderer::Stage::output), 256);
  }
  bool fastPathsMatchDirectPort(int ow = 256, int oh = 192) {
    std::vector<std::vector<float>> outs;
    for (bool fast : {false, true}) {
      auto r = renderer(CrtSettings(), ow, oh);
      if (!r) return false;
      r->useFastFFT = fast;
      r->useFastScatter = fast;
      for (int f = 0; f < 3; ++f) run(*r, codes(uint32_t(f % 3)), uint64_t(f));
      auto a = r->read(CrtRenderer::Stage::receiver), b = r->read(CrtRenderer::Stage::output);
      a.insert(a.end(), b.begin(), b.end());
      outs.push_back(a);
    }
    if (outs[0].size() != outs[1].size() || outs[0].empty()) return false;
    size_t diff = 0;
    for (size_t i = 0; i < outs[0].size(); ++i) diff += std::memcmp(&outs[0][i], &outs[1][i], 4) != 0;
    if (diff) std::printf("    %zu of %zu floats differ\n", diff, outs[0].size());
    return diff == 0;
  }
  bool deterministic() {
    std::vector<std::vector<float>> outs;
    for (int k = 0; k < 2; ++k) {
      auto r = renderer(CrtSettings());
      if (!r) return false;
      for (int f = 0; f < 4; ++f) run(*r, codes(uint32_t(f % 3)), uint64_t(f));
      outs.push_back(r->read(CrtRenderer::Stage::output));
    }
    return !outs[0].empty() && outs[0].size() == outs[1].size() && std::memcmp(outs[0].data(), outs[1].data(), outs[0].size() * 4) == 0;
  }
  const Case* find(const std::string& n) const {
    auto it = cases_.find(n);
    return it == cases_.end() ? nullptr : &it->second;
  }

 private:
  HeadlessVulkan& vk_;
  std::map<std::string, Case> cases_;
  std::vector<uint16_t> codes_ = testCodes();
};

int failures = 0;
void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

int bench(HeadlessVulkan& vk, int ow, int oh, int frames, const std::string& preset, bool rgb) {
  CrtSettings s;
  if (preset == "off") s = Conformance::off();
  if (preset == "nogrowth") s.beamGrowth = false;
  CrtRenderer r;
  std::string err;
  if (!r.init(vk.context(), VK_NULL_HANDLE, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  auto t0 = std::chrono::steady_clock::now();
  r.configure(s, ow, oh, true);
  double planMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  auto codes = testCodes();
  std::vector<uint32_t> px(256 * 240);
  for (size_t i = 0; i < px.size(); ++i) px[i] = 0xFF000000u | uint32_t((i * 2654435761u) & 0xFFFFFF);
  std::vector<double> gpu;
  double wall = 0;
  for (int f = 0; f < frames; ++f) {
    CrtRenderer::Input in;
    if (rgb) { in.kind = CrtRenderer::InputKind::rgb; in.rgb = px.data(); }
    else { in.kind = CrtRenderer::InputKind::codes; in.codes = codes.data(); in.burstPhase = uint32_t(f % 3); }
    auto a = std::chrono::steady_clock::now();
    r.runSync([&](VkCommandBuffer cmd) { r.encode(cmd, in, uint64_t(f + 1)); });
    wall += std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
    for (double g : r.takeGpuTimes()) if (f >= 5) gpu.push_back(g);
  }
  std::printf("plan: %s\n", r.planInfo().c_str());
  if (std::getenv("REPLAYNES_CRT_PROFILE")) {
    r.profile = true;
    std::map<std::string, double> acc;
    const int n = 30;
    for (int f = 0; f < n; ++f) {
      CrtRenderer::Input in;
      if (rgb) { in.kind = CrtRenderer::InputKind::rgb; in.rgb = px.data(); }
      else { in.kind = CrtRenderer::InputKind::codes; in.codes = codes.data(); in.burstPhase = uint32_t(f % 3); }
      r.runSync([&](VkCommandBuffer cmd) { r.encode(cmd, in, uint64_t(frames + f + 1)); });
      for (auto& [k, v] : r.profileTimes()) acc[k] += v / n;
    }
    for (auto& [k, v] : acc) std::printf("  %-18s %.3f ms\n", k.c_str(), v * 1000);
    r.profile = false;
  }
  std::sort(gpu.begin(), gpu.end());
  auto q = [&](double p) { return gpu.empty() ? 0.0 : gpu[size_t(std::round(double(gpu.size() - 1) * p))] * 1000; };
  double mean = 0;
  for (double g : gpu) mean += g;
  mean = gpu.empty() ? 0 : mean / double(gpu.size()) * 1000;
  std::printf("{\"device\":\"%s\",\"tube\":\"%dx%d\",\"input\":\"%s\",\"settings\":\"%s\",\"frames\":%d,\"plan_ms\":%.1f,"
              "\"gpu_ms_mean\":%.3f,\"gpu_ms_p50\":%.3f,\"gpu_ms_p90\":%.3f,\"gpu_ms_max\":%.3f,\"wall_ms_mean\":%.3f}\n",
              vk.description().c_str(), r.outputWidth(), r.outputHeight(), rgb ? "rgb" : "codes", preset.c_str(), frames, planMs,
              mean, q(0.5), q(0.9), q(1.0), wall / frames * 1000);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string fixture = RN_CRT_FIXTURE, preset = "default";
  int bw = 0, bh = 0, frames = 120;
  bool rgb = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--fixture" && i + 1 < argc) fixture = argv[++i];
    else if (a == "--bench" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &bw, &bh);
    else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (a == "--settings" && i + 1 < argc) preset = argv[++i];
    else if (a == "--rgb") rgb = true;
    else { std::fprintf(stderr, "usage: replaynes-crt-test [--fixture F] [--bench WxH --frames N --settings default|off|nogrowth --rgb]\n"); return 2; }
  }
  HeadlessVulkan vk;
  std::string err;
  if (!vk.init(&err)) {
    std::fprintf(stderr, "skipped: %s\n", err.c_str());
    return 77;
  }
  std::printf("device: %s\n", vk.description().c_str());
  if (bw > 0 && bh > 0) return bench(vk, bw, bh, frames, preset, rgb);

  std::ifstream f(fixture, std::ios::binary);
  if (!f) { std::fprintf(stderr, "missing fixture %s\n", fixture.c_str()); return 1; }
  std::stringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  Parser p{text};
  static Json root;
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
  Conformance c(vk, cases);

  // Setup model (CRTTests.testSetupModel).
  {
    double sr = 0, si = 0;
    for (double v : crt::rf::filter().real) sr += v;
    for (double v : crt::rf::filter().imag) si += v;
    check(std::fabs(sr - 1) < 1e-10 && std::fabs(si) < 1e-10, "designIF unity complex carrier gain");
    check(crt::rf::filter().delaySamples == 512 && crt::rf::kBlocks == 214, "IF delay 512, 214 FFT blocks");
    bool sums = true;
    for (int ch = 0; ch < 3; ++ch) {
      double s = 0;
      for (double v : crt::phosphor::fractions()[size_t(ch)]) s += v;
      sums &= std::fabs(s - 1) < 1e-12;
    }
    check(sums && crt::phosphor::depth() == 7, "phosphor frame fractions sum to 1, depth 7");
    auto ph = crt::composite::rowPhasesForBurst(2);
    check(ph[0] == 8 && ph[1] == 0, "burst phase -> row carrier phases");
    int w = 0, h = 0, w2 = 0, h2 = 0;
    CrtRenderer::tubeSize(3000, 2200, 0, 1600, &w, &h);
    CrtRenderer::tubeSize(800, 600, 0, 1600, &w2, &h2);
    check(w == 1600 && h2 == 600, "tube sizes bounded like OUTPUT-RESOLUTION-SPEC");
  }
  if (const Case* nh = c.find("noiseHash")) {
    const uint32_t in[4] = {0, 1, 7, 654719};
    bool ok = nh->meta && nh->meta->get("hash") && nh->meta->get("hash")->arr.size() == 4;
    for (int i = 0; ok && i < 4; ++i) ok = crt::rf::hash32(in[i]) == uint32_t(nh->meta->get("hash")->arr[size_t(i)].num);
    ok = ok && nh->meta->get("key") && crt::rf::noiseKey(1, 7) == int32_t(nh->meta->get("key")->num);
    check(ok, "noise hash32 / noiseKey match the reference");
  } else {
    check(false, "noiseHash fixture");
  }
  // Float32 GPU vs float64 CPU reference; the same bounds as the macOS CRTTests.
  Diff d;
  d = c.receiver(false);  check(d.maxAbs < 1e-4 && d.count > 10000, "receiver (AGC) " + d.str());
  d = c.receiver(true);   check(d.maxAbs < 1e-4, "receiver with antenna noise " + d.str());
  d = c.tube(false);      check(d.maxAbs < 1e-5, "tube, fixed spot " + d.str());
  d = c.tube(true);       check(d.maxAbs < 1e-5, "tube, beam growth " + d.str());
  d = c.raster160();      check(d.maxAbs < 1e-5, "reduced raster 160 lines " + d.str());
  d = c.supply();         check(d.maxAbs < 1e-3, "supply / ABL (3 frames) " + d.str());
  d = c.spotH();          check(d.maxAbs < 1e-5, "horizontal spot " + d.str());
  d = c.persistence();    check(d.maxAbs < 2e-3, "persistence (half-float ring) " + d.str());
  check(c.deterministic(), "same frame sequence is bit-identical");
  check(c.fastPathsMatchDirectPort(), "restructured kernels (shared-memory FFT/scatter, register taps) == direct port, 256x192");
  check(c.fastPathsMatchDirectPort(1144, 858), "restructured kernels == direct port, 1144x858 (Deck full screen tube)");
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
