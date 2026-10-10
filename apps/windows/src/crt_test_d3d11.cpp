// Headless conformance test of the Direct3D 11 CRT port (the checks are shared with Vulkan:
// render/crt_conformance.h) and a GPU benchmark.
//   test_crt_d3d11 [--fixture FILE] [--warp]            conformance (exit 0 pass, 1 fail, 77 no device)
//   test_crt_d3d11 --bench WxH [--frames N] [--settings default|off|nogrowth] [--rgb] [--warp]
// The device: the default hardware adapter (feature level 11_0+), or WARP with --warp /
// REPLAYNES_D3D11_WARP=1 (GitHub's runners have no GPU: their "Basic Render Driver" is WARP).
// Fixture: --fixture, else crt_reference.json next to the executable (copied there by the build),
// else the source tree path the build recorded.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "crt_d3d11.h"
#include "render/crt_conformance.h"

using namespace rnl;

namespace {

struct Device {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  bool warp = false;
  ~Device() {
    if (ctx) ctx->Release();
    if (device) device->Release();
  }
  bool init(bool wantWarp, std::string* error) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT h = E_FAIL;
    for (int attempt = wantWarp ? 1 : 0; attempt < 2 && FAILED(h); ++attempt) {
      D3D_DRIVER_TYPE type = attempt == 0 ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
      h = D3D11CreateDevice(nullptr, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, nullptr, &ctx);
      if (h == E_INVALIDARG)
        h = D3D11CreateDevice(nullptr, type, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION, &device, nullptr, &ctx);
      warp = attempt == 1;
    }
    if (FAILED(h)) {
      char b[64];
      std::snprintf(b, sizeof b, "D3D11CreateDevice (FL 11_0) failed: 0x%08lx", (unsigned long)h);
      *error = b;
      return false;
    }
    return true;
  }
};

struct D3D11Backend {
  using Renderer = CrtRendererD3D11;
  Device& dev;
  std::unique_ptr<CrtRendererD3D11> make(const CrtSettings& s, int ow, int oh, CrtQuality q) {
    auto r = std::make_unique<CrtRendererD3D11>();
    std::string err;
    if (!r->init(dev.device, dev.ctx, false, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return nullptr;
    }
    r->quality = q;
    r->configure(s, ow, oh, true);
    return r;
  }
  bool run(CrtRendererD3D11& r, const CrtInput& in, uint64_t ordinal) {
    bool encoded = r.encode(in, ordinal);
    r.finish();
    return encoded;
  }
  std::vector<float> read(CrtRendererD3D11& r, CrtStage stage) { return r.read(stage); }
};

int bench(Device& dev, int ow, int oh, int frames, const std::string& preset, bool rgb, CrtQuality quality) {
  CrtSettings s;
  if (preset == "off") s = crt_conformance::Conformance<D3D11Backend>::off();
  if (preset == "nogrowth") s.beamGrowth = false;
  CrtRendererD3D11 r;
  std::string err;
  if (!r.init(dev.device, dev.ctx, false, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  r.quality = quality;
  auto t0 = std::chrono::steady_clock::now();
  r.configure(s, ow, oh, true);
  double planMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  auto codes = crt_conformance::testCodes();
  std::vector<uint32_t> px(256 * 240);
  for (size_t i = 0; i < px.size(); ++i) px[i] = 0xFF000000u | uint32_t((i * 2654435761u) & 0xFFFFFF);
  std::vector<double> gpu;
  double wall = 0;
  for (int f = 0; f < frames; ++f) {
    CrtInput in;
    if (rgb) { in.kind = CrtInputKind::rgb; in.rgb = px.data(); }
    else { in.kind = CrtInputKind::codes; in.codes = codes.data(); in.burstPhase = uint32_t(f % 3); }
    auto a = std::chrono::steady_clock::now();
    r.encode(in, uint64_t(f + 1));
    r.finish();
    wall += std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
    for (double g : r.takeGpuTimes()) if (f >= 5) gpu.push_back(g);
  }
  // Throughput without a wait per frame (GPU-bound: the GPU time per frame when timestamps are
  // unavailable, e.g. in a VM), closed by a read-back that has to wait for the last frame.
  double through = 0;
  {
    const int n = std::max(10, frames / 2);
    auto a = std::chrono::steady_clock::now();
    for (int f = 0; f < n; ++f) {
      CrtInput in;
      if (rgb) { in.kind = CrtInputKind::rgb; in.rgb = px.data(); }
      else { in.kind = CrtInputKind::codes; in.codes = codes.data(); in.burstPhase = uint32_t(f % 3); }
      r.encode(in, uint64_t(frames + f + 1));
    }
    r.read(CrtStage::output);
    through = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count() / n;
    r.takeGpuTimes();
  }
  std::printf("plan: %s\n", r.planInfo().c_str());
  if (std::getenv("REPLAYNES_CRT_PROFILE")) {
    r.profile = true;
    std::map<std::string, double> acc;
    const int n = 30;
    for (int f = 0; f < n; ++f) {
      CrtInput in;
      if (rgb) { in.kind = CrtInputKind::rgb; in.rgb = px.data(); }
      else { in.kind = CrtInputKind::codes; in.codes = codes.data(); in.burstPhase = uint32_t(f % 3); }
      r.encode(in, uint64_t(2 * frames + f + 1));
      r.finish();
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
  std::printf("{\"device\":\"%s\",\"tube\":\"%dx%d\",\"quality\":\"%s\",\"input\":\"%s\",\"settings\":\"%s\",\"frames\":%d,\"plan_ms\":%.1f,"
              "\"compile_ms\":%.0f,\"gpu_ms_mean\":%.3f,\"gpu_ms_p50\":%.3f,\"gpu_ms_p90\":%.3f,\"gpu_ms_max\":%.3f,\"wall_ms_mean\":%.3f,"
              "\"throughput_ms\":%.3f}\n",
              r.deviceDescription().c_str(), r.outputWidth(), r.outputHeight(), r.fastActive() ? "fast" : "reference",
              rgb ? "rgb" : "codes", preset.c_str(), frames,
              planMs, r.compileSeconds() * 1000, mean, q(0.5), q(0.9), q(1.0), wall / frames * 1000, through * 1000);
  return 0;
}

/// Whether the device fuses `precise float p = a * b; precise float r = p + c;` into one
/// rounding (it must not: HLSL `precise` forbids it). a = b = 1 + 2^-12, c = -(1 + 2^-11): the
/// product rounds to 1 + 2^-11 (r = 0); fused, r = 2^-24. Inputs come from a constant buffer.
bool fusesPreciseMultiplyAdd(Device& dev) {
  const char src[] = R"(
cbuffer C : register(b0) { float a; float b; float c; float pad; };
RWByteAddressBuffer o : register(u0);
[numthreads(1, 1, 1)] void main() { precise float p = a * b; precise float r = p + c; o.Store(0, asuint(r)); }
)";
  ID3DBlob* blob = nullptr;
  if (FAILED(D3DCompile(src, sizeof src - 1, "probe", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_IEEE_STRICTNESS, 0, &blob, nullptr)))
    return false;
  ID3D11ComputeShader* cs = nullptr;
  dev.device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
  blob->Release();
  const float abc[4] = {1.0f + 1.0f / 4096, 1.0f + 1.0f / 4096, -(1.0f + 1.0f / 2048), 0};
  D3D11_BUFFER_DESC cbd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA init{abc, 0, 0};
  ID3D11Buffer* cb = nullptr;
  dev.device->CreateBuffer(&cbd, &init, &cb);
  D3D11_BUFFER_DESC ob{16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
  ID3D11Buffer* out = nullptr;
  dev.device->CreateBuffer(&ob, nullptr, &out);
  D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
  uv.Format = DXGI_FORMAT_R32_TYPELESS;
  uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uv.Buffer.NumElements = 4;
  uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  ID3D11UnorderedAccessView* uav = nullptr;
  if (out) dev.device->CreateUnorderedAccessView(out, &uv, &uav);
  D3D11_BUFFER_DESC sb{16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
  ID3D11Buffer* staging = nullptr;
  dev.device->CreateBuffer(&sb, nullptr, &staging);
  float r = 0;
  if (cs && cb && uav && staging) {
    dev.ctx->CSSetShader(cs, nullptr, 0);
    dev.ctx->CSSetConstantBuffers(0, 1, &cb);
    dev.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    dev.ctx->Dispatch(1, 1, 1);
    ID3D11UnorderedAccessView* none = nullptr;
    dev.ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    dev.ctx->CopyResource(staging, out);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(dev.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
      std::memcpy(&r, m.pData, 4);
      dev.ctx->Unmap(staging, 0);
    }
  }
  for (IUnknown* u : std::initializer_list<IUnknown*>{cs, cb, out, uav, staging})
    if (u) u->Release();
  return r != 0;
}

std::string exeDir() {
  wchar_t path[MAX_PATH * 2] = {};
  DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  return std::filesystem::path(std::wstring(path, n)).parent_path().u8string();
}

}  // namespace

int main(int argc, char** argv) {
  std::string fixture, preset = "default";
  int bw = 0, bh = 0, frames = 120;
  bool rgb = false, warp = std::getenv("REPLAYNES_D3D11_WARP") != nullptr;
  CrtQuality quality = CrtQuality::fast;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--fixture" && i + 1 < argc) fixture = argv[++i];
    else if (a == "--bench" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &bw, &bh);
    else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (a == "--settings" && i + 1 < argc) preset = argv[++i];
    else if (a == "--rgb") rgb = true;
    else if (a == "--warp") warp = true;
    else if (a == "--quality" && i + 1 < argc) quality = std::string(argv[++i]) == "reference" ? CrtQuality::reference : CrtQuality::fast;
    else {
      std::fprintf(stderr, "usage: test_crt_d3d11 [--fixture F] [--warp] [--bench WxH --frames N --settings default|off|nogrowth --rgb "
                           "--quality fast|reference]\n");
      return 2;
    }
  }
  Device dev;
  std::string err;
  if (!dev.init(warp, &err)) {
    std::fprintf(stderr, "skipped: %s\n", err.c_str());
    return 77;
  }
  {
    CrtRendererD3D11 probe;
    if (!probe.init(dev.device, dev.ctx, false, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    std::printf("device: %s%s, shaders compiled in %.0f ms\n", probe.deviceDescription().c_str(), dev.warp ? " WARP" : "",
                probe.compileSeconds() * 1000);
  }
  if (bw > 0 && bh > 0) return bench(dev, bw, bh, frames, preset, rgb, quality);
  if (fixture.empty()) {
    fixture = exeDir() + "/crt_reference.json";
#ifdef RN_CRT_FIXTURE
    if (!std::filesystem::exists(std::filesystem::u8path(fixture))) fixture = RN_CRT_FIXTURE;
#endif
  }
  const bool fuses = fusesPreciseMultiplyAdd(dev);
  if (fuses) std::printf("note: this device fuses multiply-adds marked `precise` (driver), fast == direct is checked within 2e-3\n");
  D3D11Backend backend{dev};
  int failures = crt_conformance::run(backend, fixture, fuses);
  return failures == 0 ? 0 : 1;
}
