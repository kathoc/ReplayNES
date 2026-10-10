// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md
// Direct3D 11 host side of the CRT pipeline (see crt_d3d11.h): the Vulkan host code
// (apps/linux/src/render/crt_renderer.cpp) with buffers as raw views and the immediate context.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <utility>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "crt_d3d11.h"

namespace rnl {

namespace {
#include "crt_hlsl.inc"  // kCrtHlsl[]: generated from apps/windows/shaders/crt/*.hlsl (cmake/embed_hlsl.cmake)

template <typename T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// IID_ID3D11ShaderReflection of d3dcompiler_47 (declared here: no dxguid import library needed).
const GUID kIIDShaderReflection = {0x8d536ca1, 0x0cca, 0x4956, {0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84}};

struct KernelSize {
  const char* name;
  uint32_t lx, ly;  // local size (the shader's numthreads; dispatch sizes as on Vulkan)
};
const KernelSize kKernels[] = {
    {"rf_forward_tg", 512, 1},  {"rf_inverse_tg", 512, 1},  {"rf_init_codes", 64, 4}, {"rf_butterfly2", 32, 8},
    {"rf_butterfly", 32, 8},    {"rf_multiply", 32, 8},     {"rf_extract", 32, 8},    {"rx_reduce", 64, 1},
    {"rx_agc", 1, 1},           {"rx_prepare", 32, 8},      {"rx_decode", 32, 8},     {"raster_area", 32, 8},
    {"supply_mean", 64, 1},     {"supply_row", 64, 1},      {"supply_state", 1, 1},   {"supply_resample", 32, 8},
    {"spot_h", 32, 8},          {"tube_h", 32, 8},          {"tube_v", 32, 8},        {"tube_v_growth", 32, 8},
    {"tube_scatter", 32, 8},    {"tube_scatter_x16", 32, 8}, {"tube_scatter_y16", 32, 8}, {"tube_mix", 32, 8},
    {"tube_lit", 32, 8},        {"tube_persist", 32, 8},    {"show_kernel", 16, 16},  {"tube_scatter_tx", 256, 1},
    {"tube_h_rows", 64, 1},     {"tube_lit_persist", 32, 8},
    // Fast path.
    {"rf_fast", 512, 1},        {"rx_stats_fast", 64, 1},   {"rx_agc_fast", 256, 1},  {"rx_decode_fast", 256, 1},
    {"row_mean_fast", 128, 1},  {"supply_fast", 256, 1},    {"drive_post_fast", 512, 1}, {"scatter_drive", 32, 8},
    {"tube_h_fast", 384, 1},    {"tube_v_fast", 32, 8},     {"show_kernel_h", 16, 16},
};

// Constant-buffer blocks (same layout as the GLSL push constants / the generated cbuffers).
struct RFParams { int32_t hop, overlap, delay, blocks; float scale, signalLevel, noiseSigma; int32_t noiseKey; };
struct FFTParams { int32_t span; float signValue; int32_t blocks; int32_t pad; };
struct AGCParams { float lineSeconds, attack, release, minGain, maxGain; int32_t enabled, delay, pad; };
struct RasterParams { int32_t lines, decode, rgbSource, pad; };
struct SupplyParams {
  int32_t width, rows;
  float decay, v0, reff, imax, n, relax, reqScale, ablFraction, ilim, prime;  // prime: supply_state only
  float share[4];
};
struct SpotParams { int32_t width, rows, radius; float k1; };
struct TubeParams {
  int32_t width, height, ow, oh, xCount, yCount, gCount, levels;
  float gain, scatterFraction, keep;
  int32_t radius, depth, extra, pad1, pad2;
};
struct ShowParams { float dst[4]; float src[4]; int32_t ow, oh, tw, th; float ndc[4]; };
// Fast path.
struct PostParams { int32_t supply, spot; float k1; int32_t persistence, newSlot, depth, rows, rx; };
struct ScatterParams { int32_t rows, ry, pad0, pad1; float kappa[4]; };
struct FastTubeParams {
  int32_t height, ow, oh, xCount, gCount, hStride;
  float gain, keep;
  float amb[4], light[4];
};
constexpr uint32_t kParamBytes = 64;
static_assert(sizeof(SupplyParams) == 64 && sizeof(TubeParams) == 64 && sizeof(ShowParams) == 64 && sizeof(FastTubeParams) == 64,
              "constant layout");

uint32_t groups(int n, uint32_t local) { return uint32_t((std::max(0, n) + int(local) - 1) / int(local)); }

const CrtHlslSource* hlsl(const char* name) {
  for (const CrtHlslSource& s : kCrtHlsl)
    if (std::strcmp(s.name, name) == 0) return &s;
  return nullptr;
}

ShowParams showParams(int ow, int oh, int tw, int th, const CrtRect& dst, double cropFraction) {
  double cy = double(oh) * cropFraction;
  ShowParams sp{};
  sp.dst[0] = dst.x;
  sp.dst[1] = dst.y;
  sp.dst[2] = dst.w;
  sp.dst[3] = dst.h;
  sp.src[0] = 0;
  sp.src[1] = float(cy);
  sp.src[2] = float(ow);
  sp.src[3] = float(double(oh) - 2 * cy);
  sp.ow = ow;
  sp.oh = oh;
  sp.tw = tw;
  sp.th = th;
  if (tw > 0 && th > 0) {
    sp.ndc[0] = dst.x / float(tw) * 2 - 1;
    sp.ndc[1] = dst.y / float(th) * 2 - 1;
    sp.ndc[2] = dst.w / float(tw) * 2;
    sp.ndc[3] = dst.h / float(th) * 2;
  }
  return sp;
}
}  // namespace

// ------------------------------------------------------------------ setup
bool CrtRendererD3D11::supported(ID3D11Device* device, std::string* why) {
  if (!device) {
    if (why) *why = "no device";
    return false;
  }
  if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
    if (why) *why = "needs Direct3D feature level 11_0 (compute shaders 5.0)";
    return false;
  }
  if (why) why->clear();
  return true;
}

CrtRendererD3D11::~CrtRendererD3D11() { shutdown(); }

bool CrtRendererD3D11::createBuffer(Buffer* b, size_t bytes, const void* init) {
  bytes = std::max<size_t>(16, (bytes + 3) & ~size_t(3));
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = UINT(bytes);
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  std::vector<uint8_t> padded;
  D3D11_SUBRESOURCE_DATA sd{};
  if (init) {
    sd.pSysMem = init;
    sd.SysMemPitch = UINT(bytes);
  }
  if (FAILED(device_->CreateBuffer(&bd, init ? &sd : nullptr, &b->buffer))) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = DXGI_FORMAT_R32_TYPELESS;
  sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  sv.BufferEx.NumElements = UINT(bytes / 4);
  sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
  uv.Format = DXGI_FORMAT_R32_TYPELESS;
  uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uv.Buffer.NumElements = UINT(bytes / 4);
  uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  if (FAILED(device_->CreateShaderResourceView(b->buffer, &sv, &b->srv)) ||
      FAILED(device_->CreateUnorderedAccessView(b->buffer, &uv, &b->uav))) {
    destroyBuffer(b);
    return false;
  }
  b->size = uint32_t(bytes);
  return true;
}

void CrtRendererD3D11::destroyBuffer(Buffer* b) {
  release(b->srv);
  release(b->uav);
  release(b->buffer);
  *b = Buffer();
}

void CrtRendererD3D11::upload(const Buffer& b, const void* data) { ctx_->UpdateSubresource(b.buffer, 0, nullptr, data, 0, 0); }

bool CrtRendererD3D11::compileKernels(bool withShow, std::string* error) {
  const auto t0 = std::chrono::steady_clock::now();
  // IEEE strict: no reassociation / reciprocal tricks; `precise` (from the GLSL) blocks mad fusion.
  const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS;
  // REPLAYNES_CRT_HLSL_DIR=<dir>: <dir>/<name>.hlsl instead of the embedded source (shader work).
  const char* overrideDir = std::getenv("REPLAYNES_CRT_HLSL_DIR");
  auto compile = [&](const char* name, const char* target, ID3DBlob** out) {
    const CrtHlslSource* src = hlsl(name);
    std::string text = src ? std::string(src->text, src->size) : std::string();
    if (overrideDir) {
      std::string path = std::string(overrideDir) + "/" + name + ".hlsl";
      if (FILE* f = std::fopen(path.c_str(), "rb")) {
        text.clear();
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
      }
    }
    if (text.empty()) {
      *error = std::string("missing shader ") + name;
      return false;
    }
    ID3DBlob* err = nullptr;
    HRESULT h = D3DCompile(text.data(), text.size(), name, nullptr, nullptr, "main", target, flags, 0, out, &err);
    if (FAILED(h)) {
      char b[64];
      std::snprintf(b, sizeof b, " (0x%08lx)", (unsigned long)h);
      *error = std::string("compiling ") + name + b + (err ? std::string(": ") + static_cast<const char*>(err->GetBufferPointer()) : "");
      release(err);
      return false;
    }
    release(err);
    return true;
  };
  std::string errors;  // every failing kernel, not only the first
  for (const KernelSize& k : kKernels) {
    ID3DBlob* blob = nullptr;
    if (!compile(k.name, "cs_5_0", &blob)) {
      errors += (errors.empty() ? "" : "\n") + *error;
      continue;
    }
    Kernel kern;
    HRESULT h = device_->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &kern.cs);
    // Which binding is a read-only buffer (SRV t<n>) and which a read-write one (UAV u<n>).
    ID3D11ShaderReflection* refl = nullptr;
    if (SUCCEEDED(h) && SUCCEEDED(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), kIIDShaderReflection,
                                             reinterpret_cast<void**>(&refl)))) {
      D3D11_SHADER_DESC sd{};
      refl->GetDesc(&sd);
      for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        refl->GetResourceBindingDesc(i, &bd);
        if (bd.BindPoint >= UINT(kBindings)) continue;
        if (bd.Type == D3D_SIT_BYTEADDRESS || bd.Type == D3D_SIT_STRUCTURED || bd.Type == D3D_SIT_TEXTURE)
          kern.slots[bd.BindPoint] = Bind::srv;
        else if (bd.Type == D3D_SIT_UAV_RWBYTEADDRESS || bd.Type == D3D_SIT_UAV_RWSTRUCTURED || bd.Type == D3D_SIT_UAV_RWTYPED)
          kern.slots[bd.BindPoint] = Bind::uav;
      }
      refl->Release();
    } else if (SUCCEEDED(h)) {
      h = E_FAIL;
    }
    blob->Release();
    if (FAILED(h)) {
      release(kern.cs);
      *error = std::string("kernel ") + k.name;
      return false;
    }
    kernels_[k.name] = kern;
  }
  if (!errors.empty()) {
    *error = errors;
    return false;
  }
  if (withShow) {
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    if (!compile("show_vert", "vs_5_0", &vs)) return false;
    if (!compile("show_frag", "ps_5_0", &ps)) {
      vs->Release();
      return false;
    }
    ID3DBlob* psH = nullptr;
    if (!compile("show_h_frag", "ps_5_0", &psH)) {
      vs->Release();
      ps->Release();
      return false;
    }
    device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &showVs_);
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &showPs_);
    device_->CreatePixelShader(psH->GetBufferPointer(), psH->GetBufferSize(), nullptr, &showPsH_);
    vs->Release();
    ps->Release();
    psH->Release();
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    rd.DepthClipEnable = TRUE;
    device_->CreateRasterizerState(&rd, &showRaster_);
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&bl, &showBlend_);
    if (!showVs_ || !showPs_ || !showPsH_ || !showRaster_ || !showBlend_) {
      *error = "show pipeline";
      return false;
    }
  }
  compileSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

bool CrtRendererD3D11::init(ID3D11Device* device, ID3D11DeviceContext* context, bool withShow, std::string* error) {
  std::string localError;
  std::string& err = error ? *error : localError;
  auto fail = [&](const std::string& m) {
    err = "CRT (Direct3D 11): " + m;
    shutdown();
    return false;
  };
  if (!supported(device, &err)) return fail(err);
  device_ = device;
  ctx_ = context;
  device_->AddRef();
  ctx_->AddRef();
  std::string why;
  if (!compileKernels(withShow, &why)) return fail(why);
  D3D11_BUFFER_DESC cb{};
  cb.ByteWidth = kParamBytes;
  cb.Usage = D3D11_USAGE_DYNAMIC;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device_->CreateBuffer(&cb, nullptr, &params_))) return fail("constant buffer");
  for (QuerySet& q : queries_) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    device_->CreateQuery(&qd, &q.disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    device_->CreateQuery(&qd, &q.begin);
    device_->CreateQuery(&qd, &q.end);
  }

  // Receiver (fixed size) + constant tables.
  const size_t n = crt::rf::kFFTSize, blocks = crt::rf::kBlocks;
  bool ok = createBuffer(&ping_, n * blocks * 8) && createBuffer(&pong_, n * blocks * 8) &&
            createBuffer(&kernelSpec_, crt::rf::kernelSpectrum().size() * 4, crt::rf::kernelSpectrum().data()) &&
            createBuffer(&twiddles_, crt::rf::twiddles().size() * 4, crt::rf::twiddles().data()) &&
            createBuffer(&volts_, crt::composite::voltageLUT().size() * 4, crt::composite::voltageLUT().data());
  // receiver-webgl.mjs basis: (cos, sin)(2*pi*n/12) for n < 2728, float32.
  std::vector<float> basis(2728 * 2);
  for (int i = 0; i < 2728; ++i) {
    basis[size_t(i) * 2] = float(std::cos(2 * 3.141592653589793 * double(i) / 12));
    basis[size_t(i) * 2 + 1] = float(std::sin(2 * 3.141592653589793 * double(i) / 12));
  }
  ok = ok && createBuffer(&basis_, basis.size() * 4, basis.data()) && createBuffer(&carrier_, 2728 * 240 * 8) &&
       createBuffer(&stats_, 240 * 16) && createBuffer(&gains_, 240 * 4) && createBuffer(&agcState_, 16) &&
       createBuffer(&prepared_, 682 * 240 * 16) && createBuffer(&rxOut_, 512 * 240 * 16) &&
       createBuffer(&driveIn_, 512 * 240 * 16) && createBuffer(&codes_, 256 * 240 * 2) && createBuffer(&phases_, 240 * 4) &&
       createBuffer(&rgb_, 256 * 240 * 4) && createBuffer(&weights_, size_t(crt::phosphor::depth()) * 16) &&
       createBuffer(&carrierReal_, size_t(crt::rf::kMaxSamples) * 4) &&
       createBuffer(&kernelSpecReal_, crt::rf::kernelSpectrumReal().size() * 4, crt::rf::kernelSpectrumReal().data()) &&
       createBuffer(&agcRows_, 240 * 16);
  if (!ok) return fail("buffers");
  resetReceiverPending_ = resetSupplyPending_ = true;
  ready_ = true;
  return true;
}

void CrtRendererD3D11::shutdown() {
  {
    std::lock_guard<std::mutex> lk(lock_);
    stopBuilder_ = true;
    buildCv_.notify_all();
  }
  if (builder_.joinable()) builder_.join();
  stopBuilder_ = false;
  if (builtTube_) destroyTube(builtTube_.get());
  builtTube_.reset();
  if (tube_) destroyTube(tube_.get());
  tube_.reset();
  for (Buffer* b : {&ping_, &pong_, &kernelSpec_, &twiddles_, &volts_, &basis_, &carrier_, &stats_, &gains_, &agcState_, &prepared_,
                    &rxOut_, &driveIn_, &codes_, &phases_, &rgb_, &weights_, &rasterOut_, &supplyMeans_, &supplyRow_, &supplyOut_,
                    &spotOut_, &supplyState_[0], &supplyState_[1], &showTarget_, &carrierReal_, &kernelSpecReal_, &agcRows_})
    destroyBuffer(b);
  release(showStaging_);
  showStagingSize_ = 0;
  for (auto& [name, k] : kernels_) release(k.cs);
  kernels_.clear();
  release(showVs_);
  release(showPs_);
  release(showPsH_);
  release(showRaster_);
  release(showBlend_);
  release(params_);
  for (QuerySet& q : queries_) {
    release(q.disjoint);
    release(q.begin);
    release(q.end);
  }
  queryPending_.clear();
  release(profileDisjoint_);
  for (ID3D11Query*& q : profileQueries_) release(q);
  profileQueries_.clear();
  stageLines_ = 0;
  slots_.clear();
  hasOutput_ = false;
  hasLastOrdinal_ = false;
  appliedPersistence_ = -1;
  lastDrive_ = nullptr;
  ready_ = false;
  if (ctx_) ctx_->Release();
  if (device_) device_->Release();
  ctx_ = nullptr;
  device_ = nullptr;
}

std::string CrtRendererD3D11::deviceDescription() const {
  if (!device_) return "";
  std::string name = "?";
  IDXGIDevice* dxgi = nullptr;
  IDXGIAdapter* adapter = nullptr;
  if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
      SUCCEEDED(dxgi->GetAdapter(&adapter))) {
    DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(adapter->GetDesc(&ad))) {
      int len = WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, nullptr, 0, nullptr, nullptr);
      name.assign(size_t(std::max(0, len - 1)), '\0');
      if (len > 1) WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name.data(), len, nullptr, nullptr);
    }
  }
  release(adapter);
  release(dxgi);
  const D3D_FEATURE_LEVEL fl = device_->GetFeatureLevel();
  char b[32];
  std::snprintf(b, sizeof b, " (D3D11 FL %d_%d)", (fl >> 12) & 0xF, (fl >> 8) & 0xF);
  return name + b;
}

// ------------------------------------------------------------------ configuration
void CrtRendererD3D11::configure(const CrtSettings& newSettings, int outputWidth, int outputHeight, bool synchronous) {
  CrtSettings s = newSettings.sanitized();
  if (s.lines != settings_.lines) stageLines_ = 0;  // raster/supply/spot rebuilt (nesterm: rasterChanged)
  settings_ = s;
  TubeKey key{std::max(64, outputWidth), std::max(48, outputHeight), s.lines, s.beamGrowth, s.ambientLux, quality};
  if (synchronous) {
    if (!tube_ || !(tube_->key == key)) {
      auto t = makeTube(key);
      if (t) {
        if (tube_) destroyTube(tube_.get());  // the runtime keeps buffers alive while the GPU uses them
        tube_ = std::move(t);
        appliedPersistence_ = -1;
        hasOutput_ = false;
      }
    }
    return;
  }
  std::lock_guard<std::mutex> lk(lock_);
  const TubeKey* target = hasPending_ ? &pendingKey_ : hasInFlight_ ? &inFlightKey_ : builtTube_ ? &builtTube_->key
                                                                                   : tube_ ? &tube_->key : nullptr;
  if (target && *target == key) return;
  pendingKey_ = key;
  hasPending_ = true;
  if (!builder_.joinable()) builder_ = std::thread([this] { buildLoop(); });
  buildCv_.notify_all();
}

bool CrtRendererD3D11::planPending() {
  std::lock_guard<std::mutex> lk(lock_);
  return hasPending_ || hasInFlight_ || builtTube_ != nullptr;
}

void CrtRendererD3D11::buildLoop() {
  std::unique_lock<std::mutex> lk(lock_);
  for (;;) {
    buildCv_.wait(lk, [&] { return stopBuilder_ || hasPending_; });
    if (stopBuilder_) return;
    TubeKey key = pendingKey_;
    hasPending_ = false;
    inFlightKey_ = key;
    hasInFlight_ = true;
    lk.unlock();
    auto t = makeTube(key);  // plan on the CPU + buffer creation (the device is free-threaded)
    lk.lock();
    hasInFlight_ = false;
    if (t && !hasPending_) {
      if (builtTube_) destroyTube(builtTube_.get());  // never adopted
      builtTube_ = std::move(t);
    } else if (t) {
      destroyTube(t.get());
    }
  }
}

std::unique_ptr<CrtRendererD3D11::Tube> CrtRendererD3D11::makeTube(const TubeKey& k) {
  crt::tube::Plan plan = crt::tube::makePlan(k.lines, k.ow, k.oh, k.ambient, k.growth ? 1 : 0);
  auto t = std::make_unique<Tube>();
  t->key = k;
  t->width = plan.width;
  t->height = plan.height;
  t->ow = plan.outputWidth;
  t->oh = plan.outputHeight;
  t->xCount = plan.xCount;
  t->yCount = plan.yCount;
  t->gCount = plan.gCount;
  t->levels = plan.growthLevels;
  t->radius[0] = plan.radius[0];
  t->radius[1] = plan.radius[1];
  t->gain = plan.gain;
  t->scatterFraction = plan.scatterFraction;
  t->growth = plan.growth;
  const std::vector<float>& vmap = plan.growth ? plan.gmap : plan.ymap;
  const size_t img = size_t(t->ow) * t->oh * 16;
  if (k.quality == CrtQuality::fast) {
    crt::tube::FastPlan fp = crt::tube::fastPlan(plan);
    if (plan.xCount <= 24 && fp.maxSpan <= 160) {
      crt::tube::DriveScatter sc = crt::tube::driveScatter(plan);
      t->fast = true;
      t->fastTaps = fp.taps;
      t->hStride = fp.maxSpan;
      t->srx = sc.rx;
      t->sry = sc.ry;
      for (int c = 0; c < 3; ++c) t->kappa[c] = sc.kappa[c];
      const size_t drive = size_t(512) * size_t(plan.height) * 8;
      bool ok = createBuffer(&t->xmapFast, fp.xmap.size() * 4, fp.xmap.data()) &&
                createBuffer(&t->tiles, fp.tiles.size() * 4, fp.tiles.data()) &&
                createBuffer(&t->vrows, fp.vrows.size() * 4, fp.vrows.data()) &&
                createBuffer(&t->vcoef, fp.vcoef.size() * 4, fp.vcoef.data()) &&
                createBuffer(&t->colinv, fp.colinv.size() * 4, fp.colinv.data()) &&
                createBuffer(&t->swx, sc.wx.size() * 4, sc.wx.data()) && createBuffer(&t->swy, sc.wy.size() * 4, sc.wy.data()) &&
                createBuffer(&t->hplanes, size_t(t->ow) * t->height * 2 * 16) && createBuffer(&t->outH, size_t(t->ow) * t->oh * 8) &&
                createBuffer(&t->scatterTmp, drive) && createBuffer(&t->scatterSrc, drive);
      if (!ok) {
        destroyTube(t.get());
        return nullptr;
      }
      return t;
    }
  }
  bool ok = createBuffer(&t->xmap, plan.xmap.size() * 4, plan.xmap.data()) &&
            createBuffer(&t->vmap, vmap.size() * 4, vmap.data()) &&
            createBuffer(&t->colsum, plan.colsum.size() * 4, plan.colsum.data()) &&
            createBuffer(&t->kx, plan.kernel[0].size() * 4, plan.kernel[0].data()) &&
            createBuffer(&t->ky, plan.kernel[1].size() * 4, plan.kernel[1].data()) &&
            createBuffer(&t->ambient, plan.ambient.size() * 4, plan.ambient.data()) &&
            createBuffer(&t->horizontal, size_t(t->ow) * t->height * 2 * 16) && createBuffer(&t->emission, img) &&
            createBuffer(&t->scatterX, img) && createBuffer(&t->scatterY, img);
  if (!ok) {
    destroyTube(t.get());
    return nullptr;
  }
  return t;
}

void CrtRendererD3D11::destroyTube(Tube* t) {
  if (!t) return;
  for (Buffer* b : {&t->xmap, &t->vmap, &t->colsum, &t->kx, &t->ky, &t->ambient, &t->horizontal, &t->emission, &t->scatterX,
                    &t->scatterY, &t->ring, &t->xmapFast, &t->tiles, &t->vrows, &t->vcoef, &t->colinv, &t->swx, &t->swy, &t->hplanes,
                    &t->outH, &t->scatterTmp, &t->scatterSrc})
    destroyBuffer(b);
}

bool CrtRendererD3D11::ensureStages() {
  int rows = settings_.lines;
  if (stageLines_ == rows) return true;
  for (Buffer* b : {&rasterOut_, &supplyMeans_, &supplyRow_, &supplyOut_, &spotOut_, &supplyState_[0], &supplyState_[1]}) destroyBuffer(b);
  const size_t img = size_t(512) * rows * 16;
  bool ok = createBuffer(&rasterOut_, img) && createBuffer(&supplyOut_, img) && createBuffer(&spotOut_, img) &&
            createBuffer(&supplyMeans_, size_t(rows) * 16) && createBuffer(&supplyRow_, size_t(rows) * 16) &&
            createBuffer(&supplyState_[0], 16) && createBuffer(&supplyState_[1], 16);
  if (!ok) return false;
  stageLines_ = rows;
  resetSupplyPending_ = true;
  return true;
}

std::string CrtRendererD3D11::planInfo() const {
  if (!tube_) return "";
  char b[160];
  std::snprintf(b, sizeof b, "xCount=%d yCount=%d gCount=%d levels=%d radius=%d,%d", tube_->xCount, tube_->yCount, tube_->gCount,
                tube_->levels, tube_->radius[0], tube_->radius[1]);
  return b;
}

int CrtRendererD3D11::outputWidth() const { return tube_ ? tube_->ow : 0; }
int CrtRendererD3D11::outputHeight() const { return tube_ ? tube_->oh : 0; }

// ------------------------------------------------------------------ recording
void CrtRendererD3D11::dispatch(const char* name, std::initializer_list<const Buffer*> buffers, const void* pc, uint32_t pcSize,
                                uint32_t gx, uint32_t gy) {
  const Kernel& k = kernels_.at(name);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(ctx_->Map(params_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    std::memset(m.pData, 0, kParamBytes);
    if (pc && pcSize) std::memcpy(m.pData, pc, std::min(pcSize, kParamBytes));
    ctx_->Unmap(params_, 0);
  }
  ID3D11ShaderResourceView* srvs[kBindings] = {};
  ID3D11UnorderedAccessView* uavs[kBindings] = {};
  int i = 0;
  for (const Buffer* b : buffers) {
    if (k.slots[i] == Bind::srv) srvs[i] = b->srv;
    else if (k.slots[i] == Bind::uav) uavs[i] = b->uav;
    ++i;
  }
  ctx_->CSSetShader(k.cs, nullptr, 0);
  ctx_->CSSetConstantBuffers(0, 1, &params_);
  ctx_->CSSetShaderResources(0, kBindings, srvs);
  ctx_->CSSetUnorderedAccessViews(0, kBindings, uavs, nullptr);
  ctx_->Dispatch(gx, gy, 1);
  // Unbind: the next pass may read what this one wrote (D3D11 orders the dispatches itself).
  ID3D11ShaderResourceView* noSrv[kBindings] = {};
  ID3D11UnorderedAccessView* noUav[kBindings] = {};
  ctx_->CSSetShaderResources(0, kBindings, noSrv);
  ctx_->CSSetUnorderedAccessViews(0, kBindings, noUav, nullptr);
  if (profiling_ && profileNames_.size() + 1 < profileQueries_.size()) {
    profileNames_.push_back(name);
    ctx_->End(profileQueries_[profileNames_.size()]);
  }
}

bool CrtRendererD3D11::encode(const Input& input, uint64_t ordinal) {
  if (!ready_) return false;
  {
    std::lock_guard<std::mutex> lk(lock_);
    if (builtTube_) {
      if (tube_) destroyTube(tube_.get());
      tube_ = std::move(builtTube_);
      appliedPersistence_ = -1;
      hasOutput_ = false;
    }
  }
  if (!tube_ || !ensureStages()) return false;
  if (input.kind == InputKind::codes && !input.codes) return false;
  if (input.kind == InputKind::rgb && !input.rgb) return false;
  if (input.kind == InputKind::drive && !input.drive) return false;
  Tube& t = *tube_;
  int64_t ord = int64_t(std::min<uint64_t>(ordinal, uint64_t(INT64_MAX)));
  if (hasLastOrdinal_ && ord <= lastOrdinal_) {
    resetReceiverPending_ = true;
    resetSupplyPending_ = true;
    slots_.clear();
  }
  lastOrdinal_ = ord;
  hasLastOrdinal_ = true;
  bool ringClearPending = false;
  int wantPersistence = settings_.persistence ? 1 : 0;
  if (wantPersistence != appliedPersistence_) {
    // tube.setPersistence(): allocate the ring on demand; always restarts history.
    // Reference: half4 tube-output slots. Fast: half4 drive slots (512 x lines).
    if (wantPersistence && !t.ring.buffer &&
        !createBuffer(&t.ring, size_t(crt::phosphor::depth()) * (t.fast ? size_t(512) * size_t(t.height) : size_t(t.ow) * t.oh) * 8))
      return false;
    if (wantPersistence && !t.ringCleared) ringClearPending = true;
    slots_.clear();
    appliedPersistence_ = wantPersistence;
  }
  if (settings_.persistence && t.ring.buffer && !t.ringCleared) ringClearPending = true;
  const int rows = settings_.lines;

  // GPU time of the CRT passes.
  int set = -1;
  if (queries_[0].disjoint) {
    set = queryNext_;
    queryNext_ = (queryNext_ + 1) % kQuerySets;
    queryPending_.erase(std::remove(queryPending_.begin(), queryPending_.end(), set), queryPending_.end());
    ctx_->Begin(queries_[set].disjoint);
    ctx_->End(queries_[set].begin);
  }
  profiling_ = profile;
  if (profiling_) {
    if (!profileDisjoint_) {
      D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
      device_->CreateQuery(&qd, &profileDisjoint_);
      qd.Query = D3D11_QUERY_TIMESTAMP;
      profileQueries_.assign(64, nullptr);
      for (auto& q : profileQueries_) device_->CreateQuery(&qd, &q);
    }
    profileNames_.clear();
    ctx_->Begin(profileDisjoint_);
  }
  // State resets (physical-worker 'reset': receiver.reset(), supply.reset()), in command order.
  if (resetReceiverPending_) {
    const float st[4] = {float(crt::agc::kInitialGain), 0, 0, 0};
    upload(agcState_, st);
    resetReceiverPending_ = false;
  }
  if (resetSupplyPending_) {
    const float st[4] = {float(crt::supply::kV0), 0, 1, 1};
    upload(supplyState_[0], st);
    upload(supplyState_[1], st);
    supplyCurrent_ = 0;
    resetSupplyPending_ = false;
    primeSupply_ = true;
  }
  if (ringClearPending) {
    const UINT zero[4] = {0, 0, 0, 0};
    ctx_->ClearUnorderedAccessViewUint(t.ring.uav, zero);
    t.ringCleared = true;
  }
  if (profiling_) ctx_->End(profileQueries_[0]);
  auto finish = [&] {
    if (set >= 0) {
      ctx_->End(queries_[set].end);
      ctx_->End(queries_[set].disjoint);
      queryPending_.push_back(set);
    }
    if (profiling_) ctx_->End(profileDisjoint_);
    hasOutput_ = true;
    return true;
  };
  usedCodes_ = input.kind == InputKind::codes;
  if (t.fast) {
    encodeFast(input, ordinal, ord);
    return finish();
  }

  const Buffer* drive = nullptr;
  if (input.kind == InputKind::codes) {
    upload(codes_, input.codes);
    auto ph = crt::composite::rowPhasesForBurst(input.burstPhase);
    upload(phases_, ph.data());
    // M3-NOISE: receiver thermal noise from the antenna level; seed 1, keyed by frame ordinal.
    double sigma = crt::rf::noiseSigma(crt::rf::carrierToNoiseDb(settings_.antennaDbuv), 1);
    RFParams rp{crt::rf::kHop, crt::rf::kOverlap, crt::rf::filter().delaySamples, crt::rf::kBlocks,
                float(crt::rf::kModulationDepth / (crt::composite::kWhite - crt::composite::kSync)), 1.0f,
                noiseEnabled ? float(sigma) : 0.0f, crt::rf::noiseKey(1, ordinal)};
    const uint32_t blocks = uint32_t(crt::rf::kBlocks);
    if (useFastFFT) {  // 4096 float2 = 32 KB group shared memory: the cs_5_0 maximum
      dispatch("rf_forward_tg", {&ping_, &codes_, &phases_, &volts_, &twiddles_}, &rp, sizeof rp, blocks);
      dispatch("rf_inverse_tg", {&ping_, &carrier_, &kernelSpec_, &twiddles_}, &rp, sizeof rp, blocks);
    } else {
      dispatch("rf_init_codes", {&ping_, &codes_, &phases_, &volts_}, &rp, sizeof rp, groups(4096, 64), groups(int(blocks), 4));
      const Buffer* a = &ping_;
      const Buffer* b = &pong_;
      auto fft = [&](float sign) {
        for (int span = 2; span <= 4096; span *= 4) {
          FFTParams fp{span, sign, int32_t(blocks), 0};
          const char* name = span * 2 <= 4096 ? "rf_butterfly2" : "rf_butterfly";
          dispatch(name, {a, b, &twiddles_}, &fp, sizeof fp, groups(4096, 32), groups(int(blocks), 8));
          std::swap(a, b);
        }
      };
      fft(-1);
      FFTParams mp{0, 0, int32_t(blocks), 0};
      dispatch("rf_multiply", {a, b, &kernelSpec_}, &mp, sizeof mp, groups(4096, 32), groups(int(blocks), 8));
      std::swap(a, b);
      fft(1);
      dispatch("rf_extract", {a, &carrier_}, &rp, sizeof rp, groups(2728, 32), groups(240, 8));
    }
    dispatch("rx_reduce", {&carrier_, &basis_, &stats_}, nullptr, 0, groups(240, 64));
    AGCParams ap{float(double(crt::composite::kLineSamples) / crt::rf::sampleRateHz()), float(crt::agc::kAttackSeconds),
                 float(crt::agc::kReleaseSeconds), float(crt::agc::kMinGain), float(crt::agc::kMaxGain), 1,
                 crt::rf::filter().delaySamples, 0};
    dispatch("rx_agc", {&stats_, &gains_, &agcState_}, &ap, sizeof ap, 1);
    dispatch("rx_prepare", {&carrier_, &basis_, &stats_, &prepared_}, nullptr, 0, groups(682, 32), groups(240, 8));
    dispatch("rx_decode", {&prepared_, &stats_, &gains_, &rxOut_}, nullptr, 0, groups(512, 32), groups(240, 8));
    drive = &rxOut_;
  } else if (input.kind == InputKind::rgb) {
    upload(rgb_, input.rgb);
    RasterParams rp2{rows, 1, 1, 0};
    dispatch("raster_area", {&rxOut_, &rgb_, &rasterOut_}, &rp2, sizeof rp2, groups(512, 32), groups(rows, 8));
    drive = &rasterOut_;
  } else {
    upload(driveIn_, input.drive);
    drive = &driveIn_;
  }
  if (input.kind != InputKind::rgb && rows != 240) {
    // RasterWebGL.process(texture): the native 240-line RF path passes through untouched.
    RasterParams rp2{rows, 0, 0, 0};
    dispatch("raster_area", {drive, drive, &rasterOut_}, &rp2, sizeof rp2, groups(512, 32), groups(rows, 8));
    drive = &rasterOut_;
  }

  // M4c: anode-voltage load and ABL act on the drive before the tube.
  if (settings_.supply) {
    namespace f = crt::supply;
    double line = f::kLineS * f::kVisibleLines / double(rows);
    SupplyParams sp{512,
                    rows,
                    float(std::exp(-line / f::kTauS)),
                    float(f::kV0),
                    float(f::kReff),
                    float(f::kImax),
                    float(f::kN),
                    float(std::exp(-(f::kTotalLines - f::kVisibleLines) * f::kLineS / f::kTauS)),
                    float(f::kVisibleLines / f::kTotalLines / double(rows)),
                    float(-std::expm1(-f::kFrameS / f::kTauAblS)),
                    float(f::kIlim),
                    0,
                    {float(f::kShare[0]), float(f::kShare[1]), float(f::kShare[2]), 0}};
    const Buffer* now = &supplyState_[supplyCurrent_];
    const Buffer* next = &supplyState_[1 - supplyCurrent_];
    dispatch("supply_mean", {drive, &supplyMeans_}, &sp, sizeof sp, groups(rows, 64));
    if (primeSupply_) {
      // Start of history: this frame's steady state into `next`, which then is the state in force.
      SupplyParams pp = sp;
      pp.prime = 1;
      dispatch("supply_state", {&supplyMeans_, now, next}, &pp, sizeof pp, 1);
      std::swap(now, next);
      supplyCurrent_ = 1 - supplyCurrent_;
      primeSupply_ = false;
    }
    dispatch("supply_row", {&supplyMeans_, now, &supplyRow_}, &sp, sizeof sp, groups(rows, 64));
    dispatch("supply_state", {&supplyMeans_, now, next}, &sp, sizeof sp, 1);
    dispatch("supply_resample", {drive, &supplyRow_, &supplyOut_}, &sp, sizeof sp, groups(512, 32), groups(rows, 8));
    supplyCurrent_ = 1 - supplyCurrent_;
    drive = &supplyOut_;
  }
  // M4C-SPOT-H: horizontal counterpart of the beam-current spot growth (same toggle).
  if (settings_.beamGrowth && !disableSpotH) {
    SpotParams spp{512, rows, crt::tube::kSpotHRadius, float(crt::tube::extraSigmaSamples(1, 1, 512))};
    dispatch("spot_h", {drive, &spotOut_}, &spp, sizeof spp, groups(512, 32), groups(rows, 8));
    drive = &spotOut_;
  }
  lastDrive_ = drive;

  // Tube.
  const int ow = t.ow, oh = t.oh;
  TubeParams tp{t.width, t.height, ow, oh, t.xCount, t.yCount, t.gCount, t.levels, float(t.gain), float(t.scatterFraction),
                float(1 - t.scatterFraction), 0, crt::phosphor::depth(), 0, 0, 0};
  if (useFastScatter && t.xCount <= 16)
    dispatch("tube_h_rows", {drive, &t.xmap, &t.horizontal}, &tp, sizeof tp, groups(ow, 64), 2 * uint32_t(groups(t.height, 8)));
  else
    dispatch("tube_h", {drive, &t.xmap, &t.horizontal}, &tp, sizeof tp, groups(ow, 32), groups(t.height * 2, 8));
  if (t.growth)
    dispatch("tube_v_growth", {&t.horizontal, &t.vmap, &t.colsum, &t.emission}, &tp, sizeof tp, groups(ow, 32), groups(oh, 8));
  else
    dispatch("tube_v", {&t.horizontal, &t.vmap, &t.emission}, &tp, sizeof tp, groups(ow, 32), groups(oh, 8));
  for (int axis = 0; axis < 2; ++axis) {
    tp.radius = t.radius[axis];
    tp.extra = axis;
    const Buffer* from = axis == 0 ? &t.emission : &t.scatterX;
    const Buffer* to = axis == 0 ? &t.scatterX : &t.scatterY;
    const Buffer* w = axis == 0 ? &t.kx : &t.ky;
    if (useFastScatter && axis == 0 && tp.radius <= 64) {
      dispatch("tube_scatter_tx", {from, w, to}, &tp, sizeof tp, groups(ow, 256), uint32_t(oh));
    } else if (useFastScatter) {
      if (axis == 0)
        dispatch("tube_scatter_x16", {from, w, to}, &tp, sizeof tp, groups((ow + 15) / 16, 32), groups(oh, 8));
      else
        dispatch("tube_scatter_y16", {from, w, to}, &tp, sizeof tp, groups(ow, 32), groups((oh + 15) / 16, 8));
    } else {
      dispatch("tube_scatter", {from, w, to}, &tp, sizeof tp, groups(ow, 32), groups(oh, 8));
    }
  }
  tp.extra = 0;
  // The x-scatter buffer is free again: it doubles as the output image.
  const Buffer* out = &t.scatterX;
  if (settings_.persistence && t.ring.buffer) {
    const int depth = crt::phosphor::depth();
    const int64_t next = ord;
    if (!slots_.empty() && next <= slots_.front().ordinal) slots_.clear();
    std::vector<Slot> kept;
    for (const Slot& s : slots_) {
      int64_t newer = kept.empty() ? next : kept.back().ordinal;
      if (next - newer + 1 <= int64_t(depth - 1)) kept.push_back(s);
      else break;
    }
    slots_ = kept;
    int r = 0;
    for (;;) {
      bool used = false;
      for (const Slot& s : kept) used |= s.ring == r;
      if (!used) break;
      ++r;
    }
    slots_.insert(slots_.begin(), Slot{r, next});
    std::vector<float> w = persistenceWeights();
    upload(weights_, w.data());
    tp.extra = r;
    if (useFastScatter) {
      dispatch("tube_lit_persist", {&t.emission, &t.scatterY, &t.ring, &t.ambient, out, &weights_}, &tp, sizeof tp, groups(ow, 32),
               groups(oh, 8));
    } else {
      dispatch("tube_lit", {&t.emission, &t.scatterY, &t.ring}, &tp, sizeof tp, groups(ow, 32), groups(oh, 8));
      tp.extra = 0;
      dispatch("tube_persist", {&t.ring, &t.ambient, out, &weights_}, &tp, sizeof tp, groups(ow, 32), groups(oh, 8));
    }
    tp.extra = 0;
  } else {
    dispatch("tube_mix", {&t.emission, &t.scatterY, &t.ambient, out}, &tp, sizeof tp, groups(ow, 32), groups(oh, 8));
  }
  return finish();
}

void CrtRendererD3D11::encodeFast(const Input& input, uint64_t ordinal, int64_t ord) {
  Tube& t = *tube_;
  const int rows = settings_.lines;
  const Buffer* drive = nullptr;
  bool meansReady = false;  // supplyMeans_ already holds this frame's row means (rx_decode_fast)
  if (input.kind == InputKind::codes) {
    upload(codes_, input.codes);
    auto ph = crt::composite::rowPhasesForBurst(input.burstPhase);
    upload(phases_, ph.data());
    double sigma = crt::rf::noiseSigma(crt::rf::carrierToNoiseDb(settings_.antennaDbuv), 1);
    RFParams rp{crt::rf::kHop, crt::rf::kOverlap, crt::rf::filter().delaySamples, crt::rf::kBlocks,
                float(crt::rf::kModulationDepth / (crt::composite::kWhite - crt::composite::kSync)), 1.0f,
                noiseEnabled ? float(sigma) : 0.0f, crt::rf::noiseKey(1, ordinal)};
    dispatch("rf_fast", {&carrierReal_, &codes_, &phases_, &volts_, &twiddles_, &kernelSpecReal_}, &rp, sizeof rp,
             uint32_t((crt::rf::kBlocks + 1) / 2));
    AGCParams ap{float(double(crt::composite::kLineSamples) / crt::rf::sampleRateHz()), float(crt::agc::kAttackSeconds),
                 float(crt::agc::kReleaseSeconds), float(crt::agc::kMinGain), float(crt::agc::kMaxGain), 1,
                 crt::rf::filter().delaySamples, 0};
    dispatch("rx_stats_fast", {&carrierReal_, &basis_, &stats_, &agcRows_}, &ap, sizeof ap, 240);
    dispatch("rx_agc_fast", {&agcRows_, &gains_, &agcState_}, &ap, sizeof ap, 1);
    dispatch("rx_decode_fast", {&carrierReal_, &basis_, &stats_, &gains_, &rxOut_, &supplyMeans_}, nullptr, 0, 240);
    drive = &rxOut_;
    meansReady = rows == 240;
  } else if (input.kind == InputKind::rgb) {
    upload(rgb_, input.rgb);
    RasterParams rp2{rows, 1, 1, 0};
    dispatch("raster_area", {&rxOut_, &rgb_, &rasterOut_}, &rp2, sizeof rp2, groups(512, 32), groups(rows, 8));
    drive = &rasterOut_;
  } else {
    upload(driveIn_, input.drive);
    drive = &driveIn_;
  }
  if (input.kind != InputKind::rgb && rows != 240) {
    RasterParams rp2{rows, 0, 0, 0};
    dispatch("raster_area", {drive, drive, &rasterOut_}, &rp2, sizeof rp2, groups(512, 32), groups(rows, 8));
    drive = &rasterOut_;
  }
  const Buffer* src = drive;
  const bool spot = settings_.beamGrowth && !disableSpotH;
  if (settings_.supply) {
    namespace f = crt::supply;
    double line = f::kLineS * f::kVisibleLines / double(rows);
    SupplyParams sp{512,
                    rows,
                    float(std::exp(-line / f::kTauS)),
                    float(f::kV0),
                    float(f::kReff),
                    float(f::kImax),
                    float(f::kN),
                    float(std::exp(-(f::kTotalLines - f::kVisibleLines) * f::kLineS / f::kTauS)),
                    float(f::kVisibleLines / f::kTotalLines / double(rows)),
                    float(-std::expm1(-f::kFrameS / f::kTauAblS)),
                    float(f::kIlim),
                    primeSupply_ ? 1.0f : 0.0f,
                    {float(f::kShare[0]), float(f::kShare[1]), float(f::kShare[2]), 0}};
    if (!meansReady) dispatch("row_mean_fast", {src, &supplyMeans_}, nullptr, 0, uint32_t(rows));
    dispatch("supply_fast", {&supplyMeans_, &supplyState_[supplyCurrent_], &supplyState_[1 - supplyCurrent_], &supplyRow_}, &sp,
             sizeof sp, 1);
    supplyCurrent_ = 1 - supplyCurrent_;
    primeSupply_ = false;
  }
  const bool persist = settings_.persistence && t.ring.buffer;
  PostParams pp{settings_.supply ? 1 : 0, spot ? 1 : 0, float(crt::tube::extraSigmaSamples(1, 1, 512)), 0, 0,
                crt::phosphor::depth(), rows, t.srx};
  std::vector<float> w(size_t(crt::phosphor::depth()) * 4, 0.0f);
  if (persist) {
    const int depth = crt::phosphor::depth();
    if (!slots_.empty() && ord <= slots_.front().ordinal) slots_.clear();
    std::vector<Slot> kept;
    for (const Slot& s : slots_) {
      int64_t newer = kept.empty() ? ord : kept.back().ordinal;
      if (ord - newer + 1 <= int64_t(depth - 1)) kept.push_back(s);
      else break;
    }
    slots_ = kept;
    int r = 0;
    for (;;) {
      bool used = false;
      for (const Slot& s : kept) used |= s.ring == r;
      if (!used) break;
      ++r;
    }
    slots_.insert(slots_.begin(), Slot{r, ord});
    w = persistenceWeights();
    pp.persistence = 1;
    pp.newSlot = r;
  }
  upload(weights_, w.data());
  const Buffer* ring = persist ? &t.ring : &t.scatterSrc;  // unused without persistence
  dispatch("drive_post_fast", {src, &supplyRow_, &spotOut_, ring, &weights_, &t.swx, &t.scatterTmp}, &pp, sizeof pp, uint32_t(rows));
  drive = &spotOut_;
  lastDrive_ = drive;

  // Tube.
  const double lightNorm = std::sqrt(crt::tube::kLightX * crt::tube::kLightX + crt::tube::kLightY * crt::tube::kLightY +
                                     crt::tube::kLightZ * crt::tube::kLightZ);
  FastTubeParams tp{t.height, t.ow, t.oh, t.xCount, t.fastTaps, t.hStride, float(t.gain), float(1 - t.scatterFraction),
                    {float(crt::tube::kAlbedo * t.key.ambient / (3.141592653589793 * crt::tube::kReferenceWhiteCdM2)),
                     float(crt::tube::kDiffuseFraction), float(crt::tube::kNormalSlopeX), float(crt::tube::kNormalSlopeY)},
                    {float(crt::tube::kLightX / lightNorm), float(crt::tube::kLightY / lightNorm), float(crt::tube::kLightZ / lightNorm), 0}};
  ScatterParams scp{rows, t.sry, 0, 0, {t.kappa[0], t.kappa[1], t.kappa[2], 0}};
  dispatch("scatter_drive", {&t.scatterTmp, &t.swy, &t.scatterSrc}, &scp, sizeof scp, groups(512, 32), groups(rows, 8));
  dispatch("tube_h_fast", {drive, &t.xmapFast, &t.hplanes, &t.tiles}, &tp, sizeof tp, groups(t.ow, 64), groups(t.height, 64));
  dispatch("tube_v_fast", {&t.hplanes, &t.vcoef, &t.colinv, &t.vrows, &t.scatterSrc, &t.outH}, &tp, sizeof tp, groups(t.ow, 32),
           groups(t.oh, 8));
}

/// tube-webgl.mjs persistenceWeights(): slot i (newest first) also stands for missing ordinals up
/// to the next newer slot (M4B-GAP), the oldest one for all older ordinals. Float32 accumulation
/// like the JS Float32Array.
std::vector<float> CrtRendererD3D11::persistenceWeights() const {
  const int depth = crt::phosphor::depth();
  std::vector<float> w(size_t(depth) * 4, 0.0f);
  if (slots_.empty()) return w;
  const int64_t k = slots_.front().ordinal;
  const auto& fr = crt::phosphor::fractions();
  for (size_t i = 0; i < slots_.size(); ++i) {
    const Slot& slot = slots_[i];
    int64_t newest = i == 0 ? 0 : k - slots_[i - 1].ordinal + 1, oldest = k - slot.ordinal;
    // ReplayNES: the oldest slot also stands for the ordinals before it (history start after a
    // seek / rewind / load, or the first frame): the picture is taken as held, so a still shows
    // every phosphor's full steady-state light, not just the first frame's share (purple tint).
    int64_t hi = i + 1 == slots_.size() ? int64_t(depth - 1) : std::min<int64_t>(oldest, depth - 1);
    if (newest > hi) continue;
    for (int64_t m = newest; m <= hi; ++m)
      for (int c = 0; c < 3; ++c) {
        float& v = w[size_t(slot.ring) * 4 + size_t(c)];
        v = float(double(v) + fr[size_t(c)][size_t(m)]);
      }
  }
  return w;
}

std::vector<double> CrtRendererD3D11::takeGpuTimes() {
  std::vector<double> out;
  while (!queryPending_.empty()) {
    const QuerySet& q = queries_[queryPending_.front()];
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (ctx_->GetData(q.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
    UINT64 a = 0, b = 0;
    if (ctx_->GetData(q.begin, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        ctx_->GetData(q.end, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
      break;
    queryPending_.pop_front();
    if (!dj.Disjoint && dj.Frequency > 0 && b > a) {
      double s = double(b - a) / double(dj.Frequency);
      out.push_back(s);
      lastGpu_ = s;
    }
  }
  return out;
}

std::vector<std::pair<std::string, double>> CrtRendererD3D11::profileTimes() {
  std::vector<std::pair<std::string, double>> out;
  if (!profileDisjoint_ || profileNames_.empty()) return out;
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
  while (ctx_->GetData(profileDisjoint_, &dj, sizeof dj, 0) == S_FALSE) {}
  if (dj.Disjoint || !dj.Frequency) return out;
  std::vector<UINT64> ts(profileNames_.size() + 1);
  for (size_t i = 0; i < ts.size(); ++i)
    while (ctx_->GetData(profileQueries_[i], &ts[i], sizeof(UINT64), 0) == S_FALSE) {}
  for (size_t i = 0; i < profileNames_.size(); ++i) out.push_back({profileNames_[i], double(ts[i + 1] - ts[i]) / double(dj.Frequency)});
  return out;
}

// ------------------------------------------------------------------ presentation
void CrtRendererD3D11::drawShow(int targetWidth, int targetHeight, const CrtRect& dst, double cropFraction) {
  if (!showVs_ || !tube_ || !hasOutput_ || targetWidth <= 0 || targetHeight <= 0) return;
  ShowParams sp = showParams(tube_->ow, tube_->oh, targetWidth, targetHeight, dst, cropFraction);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(ctx_->Map(params_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
  std::memcpy(m.pData, &sp, sizeof sp);
  ctx_->Unmap(params_, 0);
  D3D11_VIEWPORT v{0, 0, float(targetWidth), float(targetHeight), 0.f, 1.f};
  ctx_->RSSetViewports(1, &v);
  int x0 = std::max(0, int(std::floor(dst.x))), y0 = std::max(0, int(std::floor(dst.y)));
  int x1 = std::min(targetWidth, int(std::ceil(dst.x + dst.w))), y1 = std::min(targetHeight, int(std::ceil(dst.y + dst.h)));
  D3D11_RECT sc{x0, y0, std::max(x0, x1), std::max(y0, y1)};
  ctx_->RSSetScissorRects(1, &sc);
  ctx_->RSSetState(showRaster_);
  const float blendFactor[4] = {0, 0, 0, 0};
  ctx_->OMSetBlendState(showBlend_, blendFactor, 0xffffffffu);
  ctx_->OMSetDepthStencilState(nullptr, 0);
  ctx_->IASetInputLayout(nullptr);
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  ctx_->VSSetShader(showVs_, nullptr, 0);
  ctx_->VSSetConstantBuffers(0, 1, &params_);
  ctx_->GSSetShader(nullptr, nullptr, 0);
  ctx_->PSSetShader(tube_->fast ? showPsH_ : showPs_, nullptr, 0);
  ctx_->PSSetConstantBuffers(0, 1, &params_);
  ctx_->PSSetShaderResources(0, 1, tube_->fast ? &tube_->outH.srv : &tube_->scatterX.srv);
  ctx_->Draw(4, 0);
  ID3D11ShaderResourceView* none = nullptr;
  ctx_->PSSetShaderResources(0, 1, &none);
}

bool CrtRendererD3D11::showToBGRA(int tw, int th, const CrtRect& dst, double cropFraction, uint8_t* canvas, size_t stride) {
  if (!tube_ || !hasOutput_ || tw <= 0 || th <= 0) return false;
  const uint32_t bytes = uint32_t(tw) * uint32_t(th) * 4;
  if (showTarget_.size < bytes) {
    destroyBuffer(&showTarget_);
    release(showStaging_);
    if (!createBuffer(&showTarget_, bytes)) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = showTarget_.size;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &showStaging_))) return false;
    showStagingSize_ = showTarget_.size;
  }
  ShowParams sp = showParams(tube_->ow, tube_->oh, tw, th, dst, cropFraction);
  if (tube_->fast) dispatch("show_kernel_h", {&tube_->outH, &showTarget_}, &sp, sizeof sp, groups(tw, 16), groups(th, 16));
  else dispatch("show_kernel", {&tube_->scatterX, &showTarget_}, &sp, sizeof sp, groups(tw, 16), groups(th, 16));
  ctx_->CopyResource(showStaging_, showTarget_.buffer);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(ctx_->Map(showStaging_, 0, D3D11_MAP_READ, 0, &m))) return false;
  const uint8_t* src = static_cast<const uint8_t*>(m.pData);
  for (int y = 0; y < th; ++y) std::memcpy(canvas + size_t(y) * stride, src + size_t(y) * size_t(tw) * 4, size_t(tw) * 4);
  ctx_->Unmap(showStaging_, 0);
  return true;
}

// ------------------------------------------------------------------ synchronous helpers (tests / export)
void CrtRendererD3D11::finish() {
  if (!device_) return;
  D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
  ID3D11Query* q = nullptr;
  if (FAILED(device_->CreateQuery(&qd, &q))) {
    ctx_->Flush();
    return;
  }
  ctx_->End(q);
  BOOL done = FALSE;
  while (ctx_->GetData(q, &done, sizeof done, 0) == S_FALSE) Sleep(0);
  q->Release();
}

std::vector<float> CrtRendererD3D11::readBuffer(const Buffer& b) {
  std::vector<float> out;
  if (!b.buffer) return out;
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = b.size;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer* staging = nullptr;
  if (FAILED(device_->CreateBuffer(&bd, nullptr, &staging))) return out;
  ctx_->CopyResource(staging, b.buffer);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
    out.resize(b.size / 4);
    std::memcpy(out.data(), m.pData, b.size);
    ctx_->Unmap(staging, 0);
  }
  staging->Release();
  return out;
}

std::vector<float> CrtRendererD3D11::read(Stage stage) {
  const Buffer* src = nullptr;
  const bool fast = tube_ && tube_->fast;
  switch (stage) {
    case Stage::receiver: src = &rxOut_; break;
    case Stage::tubeInput: src = lastDrive_; break;
    case Stage::emission: src = tube_ && !fast ? &tube_->emission : nullptr; break;  // fast: not kept
    case Stage::output: src = hasOutput_ && tube_ ? (fast ? &tube_->outH : &tube_->scatterX) : nullptr; break;
  }
  if (!src) return {};
  std::vector<float> raw = readBuffer(*src);
  if (!(fast && stage == Stage::output)) return raw;
  // half4 -> float4
  std::vector<float> out(raw.size() * 2);
  const uint16_t* h = reinterpret_cast<const uint16_t*>(raw.data());
  for (size_t i = 0; i < out.size(); ++i) {
    uint32_t v = h[i], sign = (v >> 15) & 1, e = (v >> 10) & 31, m = v & 1023;
    float f = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
    out[i] = sign ? -f : f;
  }
  return out;
}

}  // namespace rnl
