// Direct3D 11 compute pipeline of the nesterm physical CRT - the Windows counterpart of the Vulkan
// CrtRenderer (apps/linux/src/render/crt_renderer.h), itself 1:1 with the macOS Metal version:
//   PPU codes -> RF/IF (FFT overlap-save FIR, M3) -> receiver + AGC (M3b/M3c, AGC on the GPU)
//   -> [raster lines] -> [supply/ABL (M4c)] -> [horizontal spot (M4C-SPOT-H)] -> tube detector +
//   scatter (M1/M4a) -> [phosphor persistence (M4b)] -> linear-light output -> show (sRGB).
// Same kernels (HLSL generated from the Vulkan GLSL by scripts/generate-crt-hlsl.sh, compiled at
// start by D3DCompile, cs/vs/ps_5_0, IEEE strict + `precise`), same pass order, constants,
// parameters/defaults, temporal state and dispatch sizes as the Vulkan host code. Storage buffers
// are raw (ByteAddress) buffers; each kernel's SRV / UAV slots come from shader reflection.
// Needs feature level 11_0 (cs_5_0, 32 KB group shared memory for the FFT). Records into the
// immediate context (no readback on the live path). Display-only: never touches emulation state.
// Not thread-safe: one thread encodes; the tube plan is built on a background thread (buffer
// creation on the free-threaded device) and adopted by a later encode.
// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "render/crt_model.h"
#include "render/crt_settings.h"
#include "render/crt_types.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Buffer;
struct ID3D11ShaderResourceView;
struct ID3D11UnorderedAccessView;
struct ID3D11ComputeShader;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11Query;
struct ID3D11RasterizerState;
struct ID3D11BlendState;

namespace rnl {

class CrtRendererD3D11 {
 public:
  using InputKind = CrtInputKind;
  using Input = CrtInput;
  using Stage = CrtStage;

  /// Feature level 11_0 or higher. Empty `why` = supported.
  static bool supported(ID3D11Device* device, std::string* why);

  CrtRendererD3D11() = default;
  ~CrtRendererD3D11();
  CrtRendererD3D11(const CrtRendererD3D11&) = delete;
  CrtRendererD3D11& operator=(const CrtRendererD3D11&) = delete;

  /// Compiles the kernels (and the show pass when `withShow`) and creates the receiver buffers.
  bool init(ID3D11Device* device, ID3D11DeviceContext* context, bool withShow, std::string* error);
  void shutdown();

  /// Applies settings and the wanted tube output size. `synchronous` builds the tube plan on the
  /// calling thread (export, tests); otherwise in the background, adopted by a later encode.
  void configure(const CrtSettings& s, int outputWidth, int outputHeight, bool synchronous = false);
  const CrtSettings& settings() const { return settings_; }
  bool planPending();

  /// One emulated frame's compute passes into the immediate context. `ordinal` = machine frame
  /// ordinal (persistence history + RF noise key); a non-increasing ordinal is a discontinuity
  /// (receiver, supply and persistence state are reset). False when no tube plan is ready yet.
  /// A frame that could not be encoded (no plan yet, stage buffers not allocatable, no input)
  /// clears hasOutput(): the caller shows the plain picture instead of the last tube output.
  bool encode(const Input& input, uint64_t ordinal);
  bool hasOutput() const { return hasOutput_; }
  /// After a failed present / device error (display watchdog): the output and the temporal state
  /// (AGC, supply, persistence ring) may be half written. The next encode starts a new history;
  /// nothing is shown until it has produced a picture.
  void discardOutput();
  /// Lines of the tube in use (the stages follow it, not settings().lines while a plan for a new
  /// line count is still being built); 0 before the first plan was adopted.
  int renderedLines() const { return tube_ ? tube_->key.lines : 0; }
  /// Times the AGC / supply state was found non-finite (NaN / inf) and reset.
  int stateResets() const { return stateResets_; }
  int outputWidth() const;
  int outputHeight() const;
  bool usedCodes() const { return usedCodes_; }
  std::string planInfo() const;

  /// showprog into the render target bound by the caller (targetWidth x targetHeight): dst =
  /// destination rectangle in target pixels (origin top-left), cropFraction = tube rows hidden at
  /// the top and bottom. Bilinear in linear light when the tube is not at the destination size.
  void drawShow(int targetWidth, int targetHeight, const CrtRect& dst, double cropFraction);
  /// showprog into a BGRA8 canvas of tw x th pixels (offline export); outside dst is black. Waits.
  bool showToBGRA(int tw, int th, const CrtRect& dst, double cropFraction, uint8_t* canvas, size_t stride);

  /// GPU seconds of every encode whose timestamps became available since the last call.
  std::vector<double> takeGpuTimes();
  double lastGpuSeconds() const { return lastGpu_; }

  /// Kernel set (CrtQuality): fast for the live view (default), reference for tests and export.
  CrtQuality quality = CrtQuality::fast;
  /// The tube in use renders with the fast kernels.
  bool fastActive() const { return tube_ && tube_->fast; }

  // Verification hooks (tests only), as in the Vulkan renderer.
  bool useFastScatter = true;
  bool useFastFFT = true;
  bool noiseEnabled = true;
  bool disableSpotH = false;
  /// The next encodes fail as if their stage buffers could not be allocated.
  int debugFailEncodes = 0;
  /// Poisons the AGC and supply state with NaN (waits for the GPU; see stateResets()).
  void debugPoisonState();
  /// Per-pass GPU timing of the next encodes (benchmark); profileTimes() after the encode.
  bool profile = false;
  std::vector<std::pair<std::string, double>> profileTimes();
  /// Copies a stage buffer back (waits for the GPU). Floats, RGBA per pixel.
  std::vector<float> read(Stage stage);
  /// Submits and waits until the GPU has finished everything recorded so far.
  void finish();
  /// Seconds init() spent compiling the shaders (diagnostics).
  double compileSeconds() const { return compileSeconds_; }
  std::string deviceDescription() const;

  static void tubeSize(double dstWidth, double dstHeight, double cropFraction, int maxWidth, int* w, int* h) {
    crtTubeSize(dstWidth, dstHeight, cropFraction, maxWidth, w, h);
  }

 private:
  struct Buffer {
    ID3D11Buffer* buffer = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    uint32_t size = 0;
  };
  struct TubeKey {
    int ow = 0, oh = 0, lines = 0;
    bool growth = false;
    double ambient = 0;
    CrtQuality quality = CrtQuality::reference;
    bool operator==(const TubeKey& o) const {
      return ow == o.ow && oh == o.oh && lines == o.lines && growth == o.growth && ambient == o.ambient && quality == o.quality;
    }
  };
  struct Tube {
    TubeKey key;
    int width = 0, height = 0, ow = 0, oh = 0, xCount = 1, yCount = 1, gCount = 1, levels = 0, radius[2] = {0, 0};
    double gain = 0, scatterFraction = 0;
    bool growth = false;
    bool ringCleared = false;
    Buffer xmap, vmap, colsum, kx, ky, ambient, horizontal, emission, scatterX, scatterY, ring;
    // Fast path (as in the Vulkan renderer): fast-plan tables, horizontal pass output, half-float
    // output, drive-domain scatter source + weights; ring = half4 drive slots (512 x lines).
    bool fast = false;
    int fastTaps = 1, hStride = 1, srx = 0, sry = 0;
    float kappa[3] = {0, 0, 0};
    Buffer xmapFast, tiles, vrows, vcoef, colinv, swx, swy, hplanes, outH, scatterTmp, scatterSrc;
  };
  struct Slot { int ring; int64_t ordinal; };
  enum class Bind : uint8_t { none, srv, uav };
  static constexpr int kBindings = 8;
  struct Kernel {
    ID3D11ComputeShader* cs = nullptr;
    Bind slots[kBindings] = {};
  };

  bool createBuffer(Buffer* b, size_t bytes, const void* init = nullptr);
  static void destroyBuffer(Buffer* b);
  std::unique_ptr<Tube> makeTube(const TubeKey& k);
  static void destroyTube(Tube* t);
  bool ensureStages(int rows);
  /// NaN / inf check of the AGC and supply state (fed back frame to frame) from a non-blocking
  /// staging copy of a recent frame: reset before the next passes when found.
  void sanitizeState();
  void copyStateForCheck();
  void buildLoop();
  std::vector<float> persistenceWeights() const;
  void upload(const Buffer& b, const void* data);
  void dispatch(const char* name, std::initializer_list<const Buffer*> buffers, const void* pc, uint32_t pcSize, uint32_t gx,
                uint32_t gy = 1);
  /// The fast path of encode() after the common prologue (same stages, state and slots).
  void encodeFast(const Input& input, uint64_t ordinal, int64_t ord);
  bool compileKernels(bool withShow, std::string* error);
  std::vector<float> readBuffer(const Buffer& b);

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* ctx_ = nullptr;
  std::map<std::string, Kernel> kernels_;
  ID3D11VertexShader* showVs_ = nullptr;
  ID3D11PixelShader* showPs_ = nullptr;
  ID3D11PixelShader* showPsH_ = nullptr;  // the fast path's half-float output
  ID3D11RasterizerState* showRaster_ = nullptr;
  ID3D11BlendState* showBlend_ = nullptr;
  ID3D11Buffer* params_ = nullptr;  // 64-byte constant buffer (the Vulkan push constants)
  double compileSeconds_ = 0;

  // GPU time: per encode a disjoint query + two timestamps.
  static constexpr int kQuerySets = 8;
  struct QuerySet { ID3D11Query* disjoint = nullptr; ID3D11Query* begin = nullptr; ID3D11Query* end = nullptr; };
  QuerySet queries_[kQuerySets];
  int queryNext_ = 0;
  std::deque<int> queryPending_;
  double lastGpu_ = 0;
  // Profiling (benchmark): a timestamp after every dispatch of one encode.
  ID3D11Query* profileDisjoint_ = nullptr;
  std::vector<ID3D11Query*> profileQueries_;
  std::vector<std::string> profileNames_;
  bool profiling_ = false;

  CrtSettings settings_;
  // Receiver (fixed size) + inputs (UpdateSubresource before the passes that read them).
  Buffer ping_, pong_, kernelSpec_, twiddles_, volts_, basis_, carrier_, stats_, gains_, agcState_, prepared_, rxOut_;
  Buffer carrierReal_, kernelSpecReal_, agcRows_;  // fast path
  Buffer codes_, phases_, rgb_, weights_, driveIn_;
  // Raster / supply / spot (per line count).
  Buffer rasterOut_, supplyMeans_, supplyRow_, supplyOut_, spotOut_, supplyState_[2];
  int supplyCurrent_ = 0;
  int stageLines_ = 0;
  bool resetReceiverPending_ = true, resetSupplyPending_ = true;
  // Start of history (first frame, or a discontinuity): the next supply pass starts from the
  // steady state of its picture (supply_state prime mode) instead of the idle tube.
  bool primeSupply_ = true;
  // Show into a canvas (export).
  Buffer showTarget_;
  ID3D11Buffer* showStaging_ = nullptr;
  uint32_t showStagingSize_ = 0;
  // Tube.
  std::unique_ptr<Tube> tube_;
  std::mutex lock_;
  std::condition_variable buildCv_;
  std::thread builder_;
  bool stopBuilder_ = false;
  bool hasPending_ = false, hasInFlight_ = false;
  TubeKey pendingKey_, inFlightKey_;
  std::unique_ptr<Tube> builtTube_;
  std::vector<Slot> slots_;
  int appliedPersistence_ = -1;
  bool hasLastOrdinal_ = false;
  int64_t lastOrdinal_ = 0;
  bool hasOutput_ = false;
  bool usedCodes_ = false;
  ID3D11Buffer* stateStaging_ = nullptr;  // agc state, supply state 0 and 1 (16 bytes each)
  bool stateReadPending_ = false;
  int stateResets_ = 0;
  const Buffer* lastDrive_ = nullptr;
  bool ready_ = false;
};

}  // namespace rnl
