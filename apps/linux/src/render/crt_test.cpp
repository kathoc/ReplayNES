// Headless conformance test of the Vulkan CRT port (the checks are shared with Direct3D 11:
// render/crt_conformance.h) and a GPU benchmark.
//   replaynes-crt-test [--fixture FILE]                 conformance (exit 0 pass, 1 fail, 77 no device)
//   replaynes-crt-test --bench WxH [--frames N] [--settings default|off|nogrowth] [--rgb]
//                      [--quality fast|reference] [--ppu FILE] [--pace HZ]
// --ppu: a frame sequence of raw PPU output (replaynes-cli dump-ppu; no ROM data) instead of the
// static test pattern; --pace: one frame per 1/HZ s like the live view (GPU clock management
// decides the clocks) instead of back to back. REPLAYNES_CRT_PROFILE=1: per-pass times too.
// REPLAYNES_VK_DEVICE=<name substring> picks the device (e.g. llvmpipe = lavapipe).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "render/crt_conformance.h"
#include "render/crt_model.h"
#include "render/crt_renderer.h"
#include "render/headless_vulkan.h"

using namespace rnl;

namespace {

struct VulkanBackend {
  using Renderer = CrtRenderer;
  HeadlessVulkan& vk;
  std::unique_ptr<CrtRenderer> make(const CrtSettings& s, int ow, int oh, CrtQuality q) {
    auto r = std::make_unique<CrtRenderer>();
    std::string err;
    if (!r->init(vk.context(), VK_NULL_HANDLE, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return nullptr;
    }
    r->quality = q;
    r->configure(s, ow, oh, true);
    return r;
  }
  bool run(CrtRenderer& r, const CrtInput& in, uint64_t ordinal) {
    bool encoded = false;
    bool ok = r.runSync([&](VkCommandBuffer cmd) { encoded = r.encode(cmd, in, ordinal); });
    return ok && encoded;
  }
  std::vector<float> read(CrtRenderer& r, CrtStage stage) { return r.read(stage); }
};

// replaynes-cli dump-ppu: "RNPPU1\0\0", u32 count, u32 0, then per frame u64 ordinal, u32 burst
// phase, u32 0, 256x240 u16 codes.
struct PpuFrame { uint32_t burst = 0; std::vector<uint16_t> codes; };
std::vector<PpuFrame> loadPpu(const std::string& path) {
  std::vector<PpuFrame> out;
  std::ifstream f(path, std::ios::binary);
  char head[16] = {};
  if (!f.read(head, 16) || std::memcmp(head, "RNPPU1", 6) != 0) return out;
  uint32_t n = 0;
  std::memcpy(&n, head + 8, 4);
  for (uint32_t i = 0; i < n; ++i) {
    char fh[16];
    PpuFrame fr;
    fr.codes.resize(256 * 240);
    if (!f.read(fh, 16) || !f.read(reinterpret_cast<char*>(fr.codes.data()), 256 * 240 * 2)) break;
    std::memcpy(&fr.burst, fh + 8, 4);
    out.push_back(std::move(fr));
  }
  return out;
}

int bench(HeadlessVulkan& vk, int ow, int oh, int frames, const std::string& preset, bool rgb, CrtQuality quality,
          const std::string& ppu, double pace) {
  CrtSettings s;
  if (preset == "off") s = rnl::crt_conformance::Conformance<VulkanBackend>::off();
  if (preset == "nogrowth") s.beamGrowth = false;
  CrtRenderer r;
  std::string err;
  if (!r.init(vk.context(), VK_NULL_HANDLE, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  r.quality = quality;
  auto t0 = std::chrono::steady_clock::now();
  r.configure(s, ow, oh, true);
  double planMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::vector<PpuFrame> seq;
  if (!ppu.empty()) {
    seq = loadPpu(ppu);
    if (seq.empty()) { std::fprintf(stderr, "bad ppu file %s\n", ppu.c_str()); return 1; }
  } else {
    for (uint32_t b = 0; b < 3; ++b) seq.push_back(PpuFrame{b, rnl::crt_conformance::testCodes()});
  }
  std::vector<uint32_t> px(256 * 240);
  for (size_t i = 0; i < px.size(); ++i) px[i] = 0xFF000000u | uint32_t((i * 2654435761u) & 0xFFFFFF);
  std::vector<double> gpu;
  double wall = 0;
  auto nextTick = std::chrono::steady_clock::now();
  auto input = [&](int f) {
    CrtRenderer::Input in;
    const PpuFrame& fr = seq[size_t(f) % seq.size()];
    if (rgb) { in.kind = CrtRenderer::InputKind::rgb; in.rgb = px.data(); }
    else { in.kind = CrtRenderer::InputKind::codes; in.codes = fr.codes.data(); in.burstPhase = fr.burst; }
    return in;
  };
  for (int f = 0; f < frames; ++f) {
    CrtRenderer::Input in = input(f);
    auto a = std::chrono::steady_clock::now();
    r.runSync([&](VkCommandBuffer cmd) { r.encode(cmd, in, uint64_t(f + 1)); });
    wall += std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
    for (double g : r.takeGpuTimes()) if (f >= 5) gpu.push_back(g);
    if (pace > 0) {
      nextTick += std::chrono::nanoseconds(int64_t(1e9 / pace));
      std::this_thread::sleep_until(nextTick);
    }
  }
  std::printf("plan: %s\n", r.planInfo().c_str());
  if (std::getenv("REPLAYNES_CRT_PROFILE")) {
    r.profile = true;
    std::map<std::string, double> acc;
    const int n = 30;
    for (int f = 0; f < n; ++f) {
      CrtRenderer::Input in = input(frames + f);
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
  std::printf("{\"device\":\"%s\",\"tube\":\"%dx%d\",\"quality\":\"%s\",\"input\":\"%s\",\"settings\":\"%s\",\"frames\":%d,"
              "\"pace_hz\":%g,\"plan_ms\":%.1f,\"gpu_ms_mean\":%.3f,\"gpu_ms_p50\":%.3f,\"gpu_ms_p90\":%.3f,\"gpu_ms_max\":%.3f,"
              "\"wall_ms_mean\":%.3f}\n",
              vk.description().c_str(), r.outputWidth(), r.outputHeight(), r.fastActive() ? "fast" : "reference",
              rgb ? "rgb" : (ppu.empty() ? "codes" : "ppu"), preset.c_str(), frames, pace, planMs, mean, q(0.5), q(0.9), q(1.0),
              wall / frames * 1000);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string fixture = RN_CRT_FIXTURE, preset = "default", ppu;
  int bw = 0, bh = 0, frames = 120;
  bool rgb = false;
  double pace = 0;
  CrtQuality quality = CrtQuality::fast;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--fixture" && i + 1 < argc) fixture = argv[++i];
    else if (a == "--bench" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &bw, &bh);
    else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (a == "--settings" && i + 1 < argc) preset = argv[++i];
    else if (a == "--rgb") rgb = true;
    else if (a == "--quality" && i + 1 < argc) quality = std::string(argv[++i]) == "reference" ? CrtQuality::reference : CrtQuality::fast;
    else if (a == "--ppu" && i + 1 < argc) ppu = argv[++i];
    else if (a == "--pace" && i + 1 < argc) pace = std::atof(argv[++i]);
    else {
      std::fprintf(stderr, "usage: replaynes-crt-test [--fixture F] [--bench WxH --frames N --settings default|off|nogrowth --rgb "
                           "--quality fast|reference --ppu FILE --pace HZ]\n");
      return 2;
    }
  }
  HeadlessVulkan vk;
  std::string err;
  if (!vk.init(&err)) {
    std::fprintf(stderr, "skipped: %s\n", err.c_str());
    return 77;
  }
  std::printf("device: %s\n", vk.description().c_str());
  if (bw > 0 && bh > 0) return bench(vk, bw, bh, frames, preset, rgb, quality, ppu, pace);

  VulkanBackend backend{vk};
  int failures = rnl::crt_conformance::run(backend, fixture);
  return failures == 0 ? 0 : 1;
}
