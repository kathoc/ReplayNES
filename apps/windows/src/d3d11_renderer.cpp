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
#include <dxgi1_5.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "crt_d3d11.h"
#include "d3d11_renderer.h"
#include "host_clock.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_sdl3.h"
#include "png_writer.h"
#include "replaynes/replaynes.h"

namespace rnl {

namespace {

template <typename T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(size_t(std::max(0, n - 1)), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string hr(const char* what, HRESULT h) {
  char b[160];
  std::snprintf(b, sizeof b, "%s failed (HRESULT 0x%08lx)", what, (unsigned long)h);
  return b;
}

// Game picture: one quad over the viewport (the GameRect); uv = visible source rectangle.
const char kShaders[] = R"(
cbuffer Params : register(b0) { float4 uvRect; };
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VsOut vs_main(uint id : SV_VertexID) {
  float2 p = float2(float(id & 1), float((id >> 1) & 1));
  VsOut o;
  o.uv = lerp(uvRect.xy, uvRect.zw, p);
  o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
  return o;
}
Texture2D picture : register(t0);
SamplerState nearest : register(s0);
float4 ps_main(VsOut i) : SV_Target { return float4(picture.Sample(nearest, i.uv).rgb, 1.0); }
)";

const char* featureLevelName(D3D_FEATURE_LEVEL l) {
  switch (l) {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    default: return "?";
  }
}

}  // namespace

D3D11Renderer::D3D11Renderer(bool vrr) : vrr_(vrr) {}

D3D11Renderer::~D3D11Renderer() { shutdown(); }

bool D3D11Renderer::init(SDL_Window* window, std::string* error) {
  window_ = window;
  hwnd_ = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
  if (!hwnd_) {
    *error = "no Win32 window handle";
    return false;
  }
  if (!createDevice(error) || !createSwapChain(error) || !createPipeline(error)) return false;
  if (!createTextures()) {
    *error = "texture creation failed";
    return false;
  }
  std::string crtWhy;
  crtSupported_ = CrtRendererD3D11::supported(device_, &crtWhy);
  status_.crtAvailable = crtSupported_;
  status_.crtError = crtWhy;
  startWaiter();
  return true;
}

void D3D11Renderer::startWaiter() {
  if (waiter_.joinable() || !swap_) return;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    stop_ = false;
  }
  waiter_ = std::thread([this] { waiterLoop(); });
}

void D3D11Renderer::stopWaiter() {
  if (!waiter_.joinable()) return;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    stop_ = true;
    cv_.notify_all();
  }
  waiter_.join();
  std::lock_guard<std::mutex> lk(mutex_);
  stop_ = false;
  toWait_.clear();  // never confirmed (the frame loop settles them as not shown)
}

void D3D11Renderer::fail(const char* what, long h) {
  std::string name = h == DXGI_ERROR_DEVICE_REMOVED  ? "DXGI_ERROR_DEVICE_REMOVED"
                     : h == DXGI_ERROR_DEVICE_RESET  ? "DXGI_ERROR_DEVICE_RESET"
                     : h == DXGI_ERROR_DEVICE_HUNG   ? "DXGI_ERROR_DEVICE_HUNG"
                     : h == DXGI_ERROR_INVALID_CALL  ? "DXGI_ERROR_INVALID_CALL"
                     : h == E_OUTOFMEMORY            ? "E_OUTOFMEMORY"
                     : h == E_INVALIDARG             ? "E_INVALIDARG"
                                                     : "HRESULT";
  bool lost = h == DXGI_ERROR_DEVICE_REMOVED || h == DXGI_ERROR_DEVICE_RESET || h == DXGI_ERROR_DEVICE_HUNG;
  if (lost && device_) {
    char b[48];
    std::snprintf(b, sizeof b, ", removed reason 0x%08lx", (unsigned long)device_->GetDeviceRemovedReason());
    name += b;
  }
  errors_.note(what, h, name);
  if (lost) deviceRemoved_ = true;
  else swapBroken_ = true;
}

void D3D11Renderer::dropCrt() {
  if (crtInit_.valid()) {  // the worker uses the device
    CrtInit pending = crtInit_.get();
    pending.crt.reset();
  }
  crt_.reset();
  crtPolicy_.reset();
}

bool D3D11Renderer::recreateSwapChain() {
  stopWaiter();  // it waits on the swap chain's waitable object
  releaseTargets();
  if (ctx_) {
    ctx_->ClearState();
    ctx_->Flush();
  }
  {
    std::lock_guard<std::mutex> lk(swapMutex_);
    if (waitable_) CloseHandle(HANDLE(waitable_));
    waitable_ = nullptr;
    release(swap_);
    currentGeneration_ = ++generation_;
  }
  std::string err;
  if (!createSwapChain(&err)) {
    errors_.note("swap chain re-creation (" + err + ")", 0);
    releaseTargets();
    if (waitable_) CloseHandle(HANDLE(waitable_));
    waitable_ = nullptr;
    release(swap_);
    return false;
  }
  swapBroken_ = false;
  startWaiter();
  std::fprintf(stderr, "ReplayNES: display: swap chain recreated (%dx%d)\n", width_, height_);
  return true;
}

void D3D11Renderer::releaseDevice() {
  stopWaiter();
  dropCrt();
  if (imguiReady_) ImGui_ImplDX11_Shutdown();
  imguiReady_ = false;
  releaseTargets();
  release(atlasSrv_);
  release(atlas_);
  release(gameSrv_);
  release(gameTex_);
  release(point_);
  release(vs_);
  release(ps_);
  release(cb_);
  release(raster_);
  release(opaque_);
  {
    std::lock_guard<std::mutex> lk(swapMutex_);
    if (waitable_) CloseHandle(HANDLE(waitable_));
    waitable_ = nullptr;
    release(swap_);
    currentGeneration_ = ++generation_;
  }
  release(factory_);
  if (ctx_) ctx_->ClearState();
  release(ctx_);
  release(device_);
  hasPicture_ = false;
  // Thumbnails are uploaded again into the new atlas.
  cellKey_.fill(0);
  cellUsed_.fill(0);
  keyCell_.clear();
  pendingThumbs_.clear();
}

bool D3D11Renderer::recreateDevice() {
  double now = nowSeconds();
  if (now < nextDeviceRetry_) return false;
  nextDeviceRetry_ = now + 1.0;
  std::fprintf(stderr, "ReplayNES: display: recreating the Direct3D 11 device (%s)\n", errors_.summary().c_str());
  releaseDevice();
  std::string err;
  bool ok = createDevice(&err) && createSwapChain(&err) && createPipeline(&err);
  if (ok && !createTextures()) {
    err = "texture creation failed";
    ok = false;
  }
  if (!ok) {
    errors_.note("Direct3D 11 device re-creation (" + err + ")", 0);
    releaseDevice();
    return false;
  }
  std::string crtWhy;
  crtSupported_ = CrtRendererD3D11::supported(device_, &crtWhy);
  status_.crtAvailable = crtSupported_;
  status_.crtError = crtWhy;
  if (imguiWanted_) {
    imguiReady_ = ImGui_ImplDX11_Init(device_, ctx_);
    if (!imguiReady_) errors_.note("ImGui_ImplDX11_Init after device re-creation", 0);
  }
  startWaiter();
  deviceRemoved_ = false;
  swapBroken_ = false;
  std::fprintf(stderr, "ReplayNES: display: Direct3D 11 device recreated (%s)\n", description_.c_str());
  return true;
}

void D3D11Renderer::recoverDisplay(bool restart) {
  if (!restart) {
    // RECOVER: the CRT output and its fed-back state are dropped; the next present shows the
    // plain picture, the one after it builds the CRT picture anew.
    if (crt_) crt_->discardOutput();
    plainNext_ = true;
    return;
  }
  // RESTART: the CRT renderer (compiled again on the worker thread) and the swap chain from
  // scratch - the device too when it was removed. CRT encodes that keep failing across a restart:
  // shown plain.
  if (crtFailStreak_ > 0 && ++crtFailedRestarts_ >= 2 && !crtGaveUp_) {
    crtGaveUp_ = true;
    status_.crtError = "CRT stopped after repeated GPU errors (switch it off and on to retry)";
    std::fprintf(stderr, "ReplayNES: display: CRT encodes keep failing after a restart: plain picture\n");
  }
  plainNext_ = true;
  if (deviceRemoved_ || !device_) {
    nextDeviceRetry_ = 0;
    recreateDevice();
    return;
  }
  dropCrt();
  recreateSwapChain();
}

std::string D3D11Renderer::displayHealth() const {
  char b[256];
  std::snprintf(b, sizeof b, "D3D11 device %s, swap chain %dx%d%s%s, CRT %s%s, CRT encodes failing %d%s",
                device_ ? (deviceRemoved_ ? "removed" : "ok") : "none", width_, height_, swapBroken_ ? " (to recreate)" : "",
                occluded_ ? " occluded" : "", crt_ ? (crt_->hasOutput() ? "output yes" : "output no") : "off",
                crt_ && crt_->planPending() ? " plan pending" : "", crtFailStreak_, crtGaveUp_ ? " (CRT given up)" : "");
  return errors_.summary() + "; " + b;
}

bool D3D11Renderer::createDevice(std::string* error) {
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  if (std::getenv("REPLAYNES_D3D11_DEBUG")) flags |= D3D11_CREATE_DEVICE_DEBUG;
  D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
  bool warp = std::getenv("REPLAYNES_D3D11_WARP") != nullptr;
  HRESULT h = E_FAIL;
  for (int attempt = warp ? 1 : 0; attempt < 2 && FAILED(h); ++attempt) {
    D3D_DRIVER_TYPE type = attempt == 0 ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
    h = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &device_,
                          &got, &ctx_);
    if (h == E_INVALIDARG)  // a runtime without 11_1
      h = D3D11CreateDevice(nullptr, type, nullptr, flags, levels + 1, UINT(std::size(levels) - 1), D3D11_SDK_VERSION,
                            &device_, &got, &ctx_);
    warp = attempt == 1;
  }
  if (FAILED(h)) {
    *error = hr("D3D11CreateDevice", h);
    return false;
  }
  IDXGIDevice* dxgiDevice = nullptr;
  IDXGIAdapter* adapter = nullptr;
  std::string adapterName = "?";
  if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) &&
      SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
    DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(adapter->GetDesc(&ad))) adapterName = narrow(ad.Description);
    adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory_));
  }
  release(adapter);
  release(dxgiDevice);
  if (!factory_) {
    *error = "no DXGI 1.2 factory";
    return false;
  }
  IDXGIFactory5* f5 = nullptr;
  if (SUCCEEDED(factory_->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5)))) {
    BOOL allow = FALSE;
    if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow))) tearingSupported_ = allow;
    f5->Release();
  }
  description_ = adapterName + " / D3D11 FL " + featureLevelName(got) + (warp ? " (WARP)" : "") +
                 " / flip discard, waitable, tearing " + (tearingSupported_ ? "supported" : "unsupported") +
                 (vrr_ ? ", --vrr" : "");
  return true;
}

bool D3D11Renderer::createSwapChain(std::string* error) {
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(window_, &w, &h);
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = UINT(std::max(1, w));
  sd.Height = UINT(std::max(1, h));
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 3;  // one on screen, one queued, one being drawn (frames may overlap)
  sd.Scaling = DXGI_SCALING_NONE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  swapFlags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  sd.Flags = swapFlags_;
  IDXGISwapChain1* sc1 = nullptr;
  HRESULT r = factory_->CreateSwapChainForHwnd(device_, HWND(hwnd_), &sd, nullptr, nullptr, &sc1);
  if (r == DXGI_ERROR_INVALID_CALL && sd.Scaling == DXGI_SCALING_NONE) {
    sd.Scaling = DXGI_SCALING_STRETCH;  // older systems
    r = factory_->CreateSwapChainForHwnd(device_, HWND(hwnd_), &sd, nullptr, nullptr, &sc1);
  }
  if (FAILED(r)) {
    *error = hr("CreateSwapChainForHwnd (flip model)", r);
    return false;
  }
  r = sc1->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&swap_));
  sc1->Release();
  if (FAILED(r)) {
    *error = hr("IDXGISwapChain2", r);
    return false;
  }
  // SDL handles full screen (borderless): no DXGI Alt+Enter / exclusive mode.
  factory_->MakeWindowAssociation(HWND(hwnd_), DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
  swap_->SetMaximumFrameLatency(1);
  waitable_ = swap_->GetFrameLatencyWaitableObject();
  // The object starts signalled once (room for one frame): take that, so every later signal is
  // one present leaving the queue.
  if (waitable_) WaitForSingleObjectEx(HANDLE(waitable_), 100, FALSE);
  width_ = int(sd.Width);
  height_ = int(sd.Height);
  return createTargets();
}

bool D3D11Renderer::createTargets() {
  if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer_)))) return false;
  return SUCCEEDED(device_->CreateRenderTargetView(backBuffer_, nullptr, &rtv_));
}

void D3D11Renderer::releaseTargets() {
  if (ctx_) ctx_->OMSetRenderTargets(0, nullptr, nullptr);
  release(rtv_);
  release(backBuffer_);
}

bool D3D11Renderer::resize(int w, int h) {
  releaseTargets();
  ctx_->Flush();
  std::lock_guard<std::mutex> lk(swapMutex_);
  HRESULT r = swap_->ResizeBuffers(0, UINT(w), UINT(h), DXGI_FORMAT_UNKNOWN, swapFlags_);
  // Presents queued before the resize are not confirmed (the waiter skips them).
  currentGeneration_ = ++generation_;
  if (FAILED(r)) {
    // Device removed / reset: the device anew; otherwise a new swap chain (next frame).
    fail("IDXGISwapChain::ResizeBuffers", long(r));
    return false;
  }
  width_ = w;
  height_ = h;
  if (!createTargets()) {
    fail("render target view after ResizeBuffers", E_FAIL);
    return false;
  }
  return true;
}

bool D3D11Renderer::createPipeline(std::string* error) {
  ID3DBlob* vsb = nullptr;
  ID3DBlob* psb = nullptr;
  ID3DBlob* err = nullptr;
  HRESULT r = D3DCompile(kShaders, sizeof kShaders - 1, "blit", nullptr, nullptr, "vs_main", "vs_4_0", 0, 0, &vsb, &err);
  if (SUCCEEDED(r)) r = D3DCompile(kShaders, sizeof kShaders - 1, "blit", nullptr, nullptr, "ps_main", "ps_4_0", 0, 0, &psb, &err);
  if (FAILED(r)) {
    *error = hr("D3DCompile", r) + (err ? std::string(": ") + static_cast<const char*>(err->GetBufferPointer()) : "");
    release(err);
    release(vsb);
    return false;
  }
  release(err);
  device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
  device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
  release(vsb);
  release(psb);
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 16;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  device_->CreateBuffer(&bd, nullptr, &cb_);
  D3D11_SAMPLER_DESC smp{};
  smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  smp.MaxLOD = D3D11_FLOAT32_MAX;
  device_->CreateSamplerState(&smp, &point_);
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.ScissorEnable = TRUE;
  rd.DepthClipEnable = TRUE;
  device_->CreateRasterizerState(&rd, &raster_);
  D3D11_BLEND_DESC bl{};
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  device_->CreateBlendState(&bl, &opaque_);
  if (!vs_ || !ps_ || !cb_ || !point_ || !raster_ || !opaque_) {
    *error = "pipeline state creation failed";
    return false;
  }
  return true;
}

bool D3D11Renderer::createTextures() {
  D3D11_TEXTURE2D_DESC td{};
  td.Width = RN_VIDEO_WIDTH;
  td.Height = RN_VIDEO_HEIGHT;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // rn_video pixels are BGRA
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DYNAMIC;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device_->CreateTexture2D(&td, nullptr, &gameTex_)) ||
      FAILED(device_->CreateShaderResourceView(gameTex_, nullptr, &gameSrv_)))
    return false;
  // Thumbnail atlas, cleared to black once (cells never show garbage).
  std::vector<uint32_t> black(size_t(kAtlasSize) * kAtlasSize, 0xFF000000u);
  D3D11_SUBRESOURCE_DATA init{black.data(), UINT(kAtlasSize * 4), 0};
  td.Width = td.Height = kAtlasSize;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.CPUAccessFlags = 0;
  return SUCCEEDED(device_->CreateTexture2D(&td, &init, &atlas_)) &&
         SUCCEEDED(device_->CreateShaderResourceView(atlas_, nullptr, &atlasSrv_));
}

bool D3D11Renderer::initImGui() {
  ImGui_ImplSDL3_InitForD3D(window_);
  imguiWanted_ = true;
  imguiReady_ = ImGui_ImplDX11_Init(device_, ctx_);
  return imguiReady_;
}

void D3D11Renderer::newImGuiFrame() {
  if (imguiReady_) ImGui_ImplDX11_NewFrame();
}

bool D3D11Renderer::fullscreenNow() const { return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0; }

bool D3D11Renderer::presentTiming() const {
  if (vrr_ && tearingSupported_ && fullscreenNow()) return false;  // host-clock pacing, free presents
  return waitable_ != nullptr;
}

uint64_t D3D11Renderer::drawAndPresent(const uint32_t* newPicture, const GameRect& rect, ImDrawData* ui, const FrameSignal* signal) {
  lastHealthy_ = false;
  occluded_ = false;
  // A removed device / broken swap chain (seen by an earlier call): recreated before anything else.
  if (deviceRemoved_ || !device_) {
    if (!recreateDevice()) return 0;
    ui = nullptr;  // built against the old device's textures; the next frame's UI uses the new ones
  }
  if ((swapBroken_ || !swap_) && !recreateSwapChain()) return 0;
  if (!post_.crt && crtGaveUp_) {  // switched off: the next switch on tries again
    crtGaveUp_ = false;
    crtFailedRestarts_ = 0;
    status_.crtError.clear();
  }
  // CRT Display on / off (created on first use - compiled on a worker thread, the device is
  // free-threaded - and freed when switched off).
  bool crtOn = post_.crt && crtSupported_ && !crtGaveUp_;
  if (crtInit_.valid() && crtInit_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    CrtInit r = crtInit_.get();
    if (r.crt) {
      std::fprintf(stderr, "CRT: Direct3D 11 pipeline ready (shaders compiled in %.0f ms, %.0f ms in all)\n",
                   r.crt->compileSeconds() * 1000, r.seconds * 1000);
      if (crtOn) {
        crt_ = std::move(r.crt);
        crtPolicy_.reset();
        status_.crtError.clear();
      }
    } else {
      status_.crtError = r.error;
      std::fprintf(stderr, "CRT: %s\n", r.error.c_str());
      post_.crt = false;
      crtOn = false;
    }
  }
  if (crtOn && !crt_ && !crtInit_.valid()) {
    ID3D11Device* device = device_;
    ID3D11DeviceContext* ctx = ctx_;
    crtInit_ = std::async(std::launch::async, [device, ctx] {
      CrtInit r;
      double t0 = nowSeconds();
      auto c = std::make_unique<CrtRendererD3D11>();
      if (c->init(device, ctx, true, &r.error)) r.crt = std::move(c);
      r.seconds = nowSeconds() - t0;
      return r;
    });
  }
  if (!crtOn && crt_) {
    crt_.reset();
    crtPolicy_.reset();
  }
  crtOn = crtOn && crt_;  // compiled and ready
  status_.crtShown = false;
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(window_, &w, &h);
  if (w <= 0 || h <= 0 || (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) return 0;
  if ((w != width_ || h != height_) && !resize(w, h)) return 0;
  if (!rtv_) {
    fail("render target view", E_FAIL);  // the swap chain anew next frame
    return 0;
  }
  double t0 = nowSeconds();
  if (newPicture) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(gameTex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      for (int y = 0; y < RN_VIDEO_HEIGHT; ++y)
        std::memcpy(static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, newPicture + size_t(y) * RN_VIDEO_WIDTH,
                    size_t(RN_VIDEO_WIDTH) * 4);
      ctx_->Unmap(gameTex_, 0);
      hasPicture_ = true;
    }
  }
  uploadThumbs();
  lastAcquireWait_ = nowSeconds() - t0;  // Map may wait for the GPU (the picture in use)

  // CRT: a new picture (or a changed size / setting / tube plan) is built by its compute passes,
  // here before the show pass (or, built ahead, after this present: the next one shows it).
  bool crtBuildAfter = false;
  bool crtFailed = false;  // a CRT encode with a plan in use did nothing (its output was cleared)
  const bool plain = plainNext_;  // display watchdog RECOVER: the plain picture for this present
  plainNext_ = false;
  if (crtOn) {
    crtPolicy_.update(crt_->takeGpuTimes(), gpuBudget_, post_.allowBuildAhead, post_.adaptiveResolution, crt_->outputWidth());
    if (newPicture) crtPolicy_.store(newPicture, signal);
    if (rect.visible) {
      int tw = 0, th = 0;
      crtPolicy_.tubeSize(post_, rect.crt, rect.crtCrop, &tw, &th);
      crt_->configure(post_.crtSettings, tw, th);
    }
    bool changed = shownPost_ != post_ || shownCrt_.w != rect.crt.w || shownCrt_.h != rect.crt.h;
    bool needBuild = !plain && crtPolicy_.hasFrame() && (newPicture || changed || !crt_->hasOutput() || crt_->planPending());
    if (needBuild) {
      if (crtPolicy_.pipelined() && crt_->hasOutput()) crtBuildAfter = true;
      else if (!crt_->encode(crtPolicy_.input(), crtPolicy_.ordinal()) && crt_->renderedLines() > 0) crtFailed = true;
    }
    shownPost_ = post_;
    shownCrt_ = rect.crt;
  }

  ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
  const float black[4] = {0, 0, 0, 1};
  ctx_->ClearRenderTargetView(rtv_, black);
  if (crtOn && rect.visible && crt_->hasOutput() && !plain) {
    crt_->drawShow(width_, height_, rect.crt, rect.crtCrop);
    status_.crtShown = true;
  } else if (rect.visible && hasPicture_) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      float uv[4] = {float(rect.crop) / RN_VIDEO_WIDTH, float(rect.crop) / RN_VIDEO_HEIGHT,
                     float(RN_VIDEO_WIDTH - rect.crop) / RN_VIDEO_WIDTH, float(RN_VIDEO_HEIGHT - rect.crop) / RN_VIDEO_HEIGHT};
      std::memcpy(m.pData, uv, sizeof uv);
      ctx_->Unmap(cb_, 0);
    }
    D3D11_VIEWPORT vp{rect.x, rect.y, rect.w, rect.h, 0.f, 1.f};
    ctx_->RSSetViewports(1, &vp);
    D3D11_RECT sc{LONG(std::max(0.f, rect.x)), LONG(std::max(0.f, rect.y)), LONG(std::min(float(width_), rect.x + rect.w)),
                  LONG(std::min(float(height_), rect.y + rect.h))};
    ctx_->RSSetScissorRects(1, &sc);
    ctx_->RSSetState(raster_);
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx_->OMSetBlendState(opaque_, blendFactor, 0xffffffffu);
    ctx_->OMSetDepthStencilState(nullptr, 0);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, &cb_);
    ctx_->PSSetShader(ps_, nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, &gameSrv_);
    ctx_->PSSetSamplers(0, 1, &point_);
    ctx_->GSSetShader(nullptr, nullptr, 0);
    ctx_->Draw(4, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx_->PSSetShaderResources(0, 1, &none);
  }
  if (ui && imguiReady_ && ui->CmdListsCount > 0) ImGui_ImplDX11_RenderDrawData(ui);
  if (!screenshotPath_.empty()) saveScreenshot();

  const bool timing = presentTiming();
  const bool tearing = vrr_ && tearingSupported_ && fullscreenNow();
  uint64_t id = 0;
  UINT count = 0;
  {
    std::lock_guard<std::mutex> lk(swapMutex_);
    double p0 = nowSeconds();
    HRESULT r = swap_->Present(tearing ? 0 : 1, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    lastAcquireWait_ += nowSeconds() - p0;  // Present blocks when every buffer is in use
    if (FAILED(r)) {
      fail("IDXGISwapChain::Present", long(r));  // device removed / reset: recreated next frame
      if (crt_) crt_->discardOutput();
      return 0;
    }
    if (r == DXGI_STATUS_OCCLUDED) {  // nothing shown: keep time on the host clock
      occluded_ = true;
      return 0;
    }
    swap_->GetLastPresentCount(&count);
    id = ++presentId_;
  }
  if (crtBuildAfter) {
    // Built ahead: queued after this present's commands (the immediate context keeps the order),
    // so the next present shows it.
    if (!crt_->encode(crtPolicy_.input(), crtPolicy_.ordinal()) && crt_->renderedLines() > 0) crtFailed = true;
    ctx_->Flush();
  }
  if (crtFailed) {
    crtFailStreak_ += 1;
  } else if (crtOn && crt_->hasOutput()) {
    crtFailStreak_ = 0;
    crtFailedRestarts_ = 0;
  }
  lastHealthy_ = !crtFailed;
  if (crtOn) {
    crtPolicy_.fillStatus(&status_);
    status_.tubeWidth = crt_->outputWidth();
    status_.tubeHeight = crt_->outputHeight();
    status_.usedCodes = crt_->usedCodes();
  }
  if (timing) {
    std::lock_guard<std::mutex> lk(mutex_);
    toWait_.push_back(Pending{id, count, generation_});
    cv_.notify_one();
  } else if (waitable_) {
    // Not waited for: keep the object's count from growing (it is signalled per present).
    WaitForSingleObjectEx(HANDLE(waitable_), 0, FALSE);
  }
  return id;
}

void D3D11Renderer::waiterLoop() {
  // REPLAYNES_DEBUG_PRESENT=1: one line per present (frame statistics, chosen time).
  const bool trace = std::getenv("REPLAYNES_DEBUG_PRESENT") != nullptr;
  struct Waiting {
    Pending p;
    double since;  // nowSeconds() when the waiter took it
  };
  std::deque<Waiting> waiting;
  // Recent frame statistics samples: present count -> the vblank it was shown on.
  std::deque<std::pair<UINT, double>> shown;
  std::deque<std::pair<UINT, double>> vblanks;  // (SyncRefreshCount, SyncQPCTime)
  bool statsWork = false;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(mutex_);
      if (waiting.empty()) cv_.wait(lk, [&] { return stop_ || !toWait_.empty(); });
      if (stop_) return;
      double now = nowSeconds();
      while (!toWait_.empty()) {
        waiting.push_back(Waiting{toWait_.front(), now});
        toWait_.pop_front();
      }
    }
    // A frame leaving the queue signals the object (a wake-up hint); poll every 2 ms meanwhile.
    DWORD r = WaitForSingleObjectEx(HANDLE(waitable_), 2, FALSE);
    const double woke = nowSeconds();
    DXGI_FRAME_STATISTICS st{};
    HRESULT sr;
    {
      std::lock_guard<std::mutex> lk(swapMutex_);
      sr = swap_->GetFrameStatistics(&st);
    }
    if (SUCCEEDED(sr) && st.SyncQPCTime.QuadPart != 0) {
      statsWork = true;
      // SyncQPCTime is the vblank SyncRefreshCount; the present was shown PresentRefreshCount.
      double t = qpcToSeconds(st.SyncQPCTime.QuadPart);
      if (shown.empty() || shown.back().first != st.PresentCount) {
        shown.emplace_back(st.PresentCount, t);
        if (shown.size() > 32) shown.pop_front();
      }
      // Refresh period: vblank count and time differences over 1-3 s.
      if (vblanks.empty() || st.SyncRefreshCount != vblanks.back().first) {
        if (!vblanks.empty() && (st.SyncRefreshCount < vblanks.back().first || t <= vblanks.back().second))
          vblanks.clear();  // display mode / adapter change
        vblanks.emplace_back(st.SyncRefreshCount, t);
        while (vblanks.size() > 2 && vblanks.back().second - vblanks[1].second >= 1.0) vblanks.pop_front();
        const auto& a = vblanks.front();
        const auto& b = vblanks.back();
        if (b.second - a.second >= 1.0 && b.first > a.first) {
          double period = (b.second - a.second) / double(b.first - a.first);
          if (period > 1.0 / 500 && period < 1.0 / 20) refreshPeriod_ = period;
        }
      }
    }
    std::vector<PresentDone> out;
    const uint64_t gen = currentGeneration_.load();
    while (!waiting.empty()) {
      const Waiting& w = waiting.front();
      PresentDone d;
      d.id = w.p.id;
      d.result = int(r);
      if (w.p.generation != gen) {
        // Resized meanwhile: not confirmed.
      } else if (statsWork) {
        if (shown.empty() || shown.back().first < w.p.presentCount) {
          if (woke - w.since < 0.25) break;  // not shown yet
        } else {
          for (const auto& [pc, t] : shown)
            if (pc == w.p.presentCount) {
              d.ok = true;
              d.time = t;
            }
          // A present the statistics skipped (replaced by a later one in the same vblank, or
          // sampled too late): not confirmed.
        }
      } else if (r == WAIT_OBJECT_0) {
        // No frame statistics (some drivers / remote sessions): the signal itself, like
        // vkWaitForPresentKHR returning.
        d.ok = true;
        d.time = woke;
      } else if (woke - w.since < 0.25) {
        break;
      }
      if (trace)
        std::fprintf(stderr, "present id=%llu count=%u wait=%d woke=%.6f stats=%s pc=%u prc=%u src=%u sync=%.6f -> ok=%d t=%.6f\n",
                     (unsigned long long)d.id, unsigned(w.p.presentCount), d.result, woke, SUCCEEDED(sr) ? "ok" : "no",
                     st.PresentCount, st.PresentRefreshCount, st.SyncRefreshCount,
                     st.SyncQPCTime.QuadPart ? qpcToSeconds(st.SyncQPCTime.QuadPart) : 0.0, d.ok ? 1 : 0, d.time);
      out.push_back(d);
      waiting.pop_front();
      if (!statsWork) break;  // one signal, one present
    }
    if (!out.empty()) {
      std::lock_guard<std::mutex> lk(mutex_);
      done_.insert(done_.end(), out.begin(), out.end());
    }
  }
}

std::vector<PresentDone> D3D11Renderer::takeCompleted() {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<PresentDone> out;
  out.swap(done_);
  return out;
}

bool D3D11Renderer::thumbTexture(uint64_t key, const uint32_t* px, uint64_t* texId, float uv[4]) {
  if (!atlasSrv_) return false;
  int cell = -1;
  auto it = keyCell_.find(key);
  if (it != keyCell_.end()) {
    cell = it->second;
  } else {
    if (int(pendingThumbs_.size()) >= kAtlasUploadsPerFrame) return false;
    // A free cell, else the least recently used one not drawn this frame.
    uint64_t best = ~uint64_t(0);
    for (int i = 0; i < kAtlasCells; ++i) {
      if (cellKey_[size_t(i)] == 0) {
        cell = i;
        break;
      }
      if (cellUsed_[size_t(i)] < uiFrame_ && cellUsed_[size_t(i)] < best) {
        best = cellUsed_[size_t(i)];
        cell = i;
      }
    }
    if (cell < 0) return false;
    if (cellKey_[size_t(cell)] != 0) keyCell_.erase(cellKey_[size_t(cell)]);
    cellKey_[size_t(cell)] = key;
    keyCell_[key] = cell;
    pendingThumbs_.erase(std::remove_if(pendingThumbs_.begin(), pendingThumbs_.end(),
                                        [&](const PendingThumb& p) { return p.cell == cell; }),
                         pendingThumbs_.end());
    pendingThumbs_.push_back(PendingThumb{cell, std::vector<uint32_t>(px, px + 128 * 120)});
  }
  cellUsed_[size_t(cell)] = uiFrame_;
  const int perRow = kAtlasSize / kAtlasCell;
  float x0 = float((cell % perRow) * kAtlasCell), y0 = float((cell / perRow) * kAtlasCell);
  uv[0] = x0 / kAtlasSize;
  uv[1] = y0 / kAtlasSize;
  uv[2] = (x0 + 128) / kAtlasSize;
  uv[3] = (y0 + 120) / kAtlasSize;
  *texId = uint64_t(reinterpret_cast<uintptr_t>(atlasSrv_));  // imgui_impl_dx11: ImTextureID = SRV*
  return true;
}

void D3D11Renderer::uploadThumbs() {
  const int perRow = kAtlasSize / kAtlasCell;
  for (const PendingThumb& p : pendingThumbs_) {
    D3D11_BOX box{UINT((p.cell % perRow) * kAtlasCell), UINT((p.cell / perRow) * kAtlasCell), 0, 0, 0, 1};
    box.right = box.left + 128;
    box.bottom = box.top + 120;
    ctx_->UpdateSubresource(atlas_, 0, &box, p.px.data(), 128 * 4, 0);
  }
  pendingThumbs_.clear();
}

void D3D11Renderer::saveScreenshot() {
  std::string path;
  path.swap(screenshotPath_);
  D3D11_TEXTURE2D_DESC td{};
  backBuffer_->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging))) return;
  ctx_->CopyResource(staging, backBuffer_);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
    int w = int(td.Width), h = int(td.Height);
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
      const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
      uint8_t* dst = rgba.data() + size_t(y) * w * 4;
      for (int x = 0; x < w; ++x) {  // BGRA -> RGBA, opaque
        dst[x * 4 + 0] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 0];
        dst[x * 4 + 3] = 255;
      }
    }
    ctx_->Unmap(staging, 0);
    if (writePNG(path, w, h, rgba.data())) std::fprintf(stderr, "screenshot: %s (%dx%d)\n", path.c_str(), w, h);
    else std::fprintf(stderr, "screenshot FAILED: %s\n", path.c_str());
  }
  staging->Release();
}

void D3D11Renderer::shutdown() { releaseDevice(); }

}  // namespace rnl
