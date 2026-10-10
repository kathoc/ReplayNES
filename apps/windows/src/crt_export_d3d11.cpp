// "Apply CRT effect" for the MP4 export on Windows (render/crt_export.h): the live CRT pipeline
// (CrtRendererD3D11) on its own window-less Direct3D 11 device (hardware, else WARP), synchronous
// tube plan, one frame at a time in order, read back into the encoder's canvas - the counterpart
// of apps/linux/src/render/crt_export.cpp. Display only: the renderer hash is unaffected.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <memory>

#include "crt_d3d11.h"
#include "render/crt_export.h"

namespace rnl {

namespace {
class CrtExportProcessorD3D11 : public ExportVideoProcessor {
 public:
  explicit CrtExportProcessorD3D11(const CrtSettings& s) : settings_(s) {}
  ~CrtExportProcessorD3D11() override {
    renderer_.reset();
    if (ctx_) ctx_->Release();
    if (device_) device_->Release();
  }

  bool init(std::string* error) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT h = E_FAIL;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
      h = D3D11CreateDevice(nullptr, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
      if (h == E_INVALIDARG) h = D3D11CreateDevice(nullptr, type, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
      if (SUCCEEDED(h)) break;
    }
    if (FAILED(h)) {
      char b[96];
      std::snprintf(b, sizeof b, "CRT effect: no Direct3D 11 device with feature level 11_0 (0x%08lx)", (unsigned long)h);
      *error = b;
      return false;
    }
    renderer_ = std::make_unique<CrtRendererD3D11>();
    return renderer_->init(device_, ctx_, false, error);
  }

  bool begin(const rnf_export_settings& s, const rnf_export_geometry& g, std::string*) override {
    w_ = g.canvas_width;
    h_ = g.canvas_height;
    crop_ = double(s.crop_top + s.crop_bottom) / 2 / 240;
    // Full 4:3 raster minus the cropped overscan rows, fitted into the canvas and centred.
    double aspect = (4.0 / 3.0) / (1 - 2 * crop_);
    double h = double(h_), w = std::round(h * aspect);
    if (w > double(w_)) {
      w = double(w_);
      h = std::round(w / aspect);
    }
    dst_ = CrtRect{float(std::round((double(w_) - w) / 2)), float(std::round((double(h_) - h) / 2)), float(w), float(h)};
    int tw = 0, th = 0;
    crtTubeSize(w, h, crop_, 1600, &tw, &th);
    renderer_->quality = CrtQuality::reference;  // offline: the 1:1 port (time is not critical)
    renderer_->configure(settings_, tw, th, true);
    return true;
  }

  // Same input rule as the live view: RF path from the PPU codes, or the (flash-filtered) RGB
  // picture when the filter altered the frame / the core has no codes.
  bool render(const ExportFrameSignal& sig, const uint32_t* pixels, uint8_t* canvas, size_t stride, std::string* error) override {
    CrtInput in;
    if (sig.codes && !sig.flashAltered) {
      in.kind = CrtInputKind::codes;
      in.codes = sig.codes;
      in.burstPhase = sig.burstPhase;
    } else {
      in.kind = CrtInputKind::rgb;
      in.rgb = pixels;
    }
    if (!renderer_->encode(in, sig.ordinal) || !renderer_->showToBGRA(w_, h_, dst_, crop_, canvas, stride)) {
      if (error) *error = "CRT rendering failed";
      return false;
    }
    return true;
  }

 private:
  CrtSettings settings_;
  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* ctx_ = nullptr;
  std::unique_ptr<CrtRendererD3D11> renderer_;
  int w_ = 0, h_ = 0;
  double crop_ = 0;
  CrtRect dst_;
};
}  // namespace

std::unique_ptr<ExportVideoProcessor> makeCrtExportProcessor(const CrtSettings& settings, std::string* error) {
  auto p = std::make_unique<CrtExportProcessorD3D11>(settings);
  std::string err;
  if (!p->init(&err)) {
    if (error) *error = err;
    return nullptr;
  }
  return p;
}

std::function<std::unique_ptr<ExportVideoProcessor>(std::string*)> crtExportProcessorFactory(const CrtSettings& settings) {
  return [settings](std::string* error) { return makeCrtExportProcessor(settings, error); };
}

}  // namespace rnl
