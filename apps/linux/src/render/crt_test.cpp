// Headless conformance test of the Vulkan CRT port (the checks are shared with Direct3D 11:
// render/crt_conformance.h) and a GPU benchmark.
//   replaynes-crt-test [--fixture FILE]                 conformance (exit 0 pass, 1 fail, 77 no device)
//   replaynes-crt-test --bench WxH [--frames N] [--settings default|off|nogrowth] [--rgb]
// REPLAYNES_VK_DEVICE=<name substring> picks the device (e.g. llvmpipe = lavapipe).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
  std::unique_ptr<CrtRenderer> make(const CrtSettings& s, int ow, int oh) {
    auto r = std::make_unique<CrtRenderer>();
    std::string err;
    if (!r->init(vk.context(), VK_NULL_HANDLE, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return nullptr;
    }
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

int bench(HeadlessVulkan& vk, int ow, int oh, int frames, const std::string& preset, bool rgb) {
  CrtSettings s;
  if (preset == "off") s = rnl::crt_conformance::Conformance<VulkanBackend>::off();
  if (preset == "nogrowth") s.beamGrowth = false;
  CrtRenderer r;
  std::string err;
  if (!r.init(vk.context(), VK_NULL_HANDLE, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  auto t0 = std::chrono::steady_clock::now();
  r.configure(s, ow, oh, true);
  double planMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  auto codes = rnl::crt_conformance::testCodes();
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

  VulkanBackend backend{vk};
  int failures = rnl::crt_conformance::run(backend, fixture);
  return failures == 0 ? 0 : 1;
}
