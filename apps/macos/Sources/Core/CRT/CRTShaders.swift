// Metal (MSL, compiled at runtime from source) ports of the nesterm / crt-physical-model WebGL2
// shaders. Each kernel names its source shader; arithmetic, constants and evaluation order follow
// the GLSL 1:1 (GLSL mix() is spelled out as x*(1-a)+y*a; fast math is disabled by the caller).
// Images are float4 buffers, row 0 = top (the WebGL textures' y=0 row), index y*width+x.
// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md

enum CRTShaders {
    /// Reference kernels (the 1:1 port and its bit-identical restructurings) + the fast path.
    static let source = reference + fast

    static let reference = #"""
#include <metal_stdlib>
using namespace metal;

static inline float3 glmix3(float3 x, float3 y, float a) { return x * (1.0f - a) + y * a; }
static inline float glmix1(float x, float y, float a) { return x * (1.0f - a) + y * a; }

// ---------------------------------------------------------------- rf-webgl.mjs
static inline int reverse12(int x) { int y = 0; for (int b = 0; b < 12; b++) { y = (y << 1) | (x & 1); x >>= 1; } return y; }
static inline float2 cmul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
static inline uint lowbias(uint x) { x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u; x *= 0x846ca68bu; x ^= x >> 16u; return x; }
static inline float gaussAt(int pos, int key) {
    uint k = uint(key);
    float u1 = (float(lowbias((uint(pos) * 2u) ^ k)) + 0.5f) / 4294967296.0f;
    float u2 = (float(lowbias((uint(pos) * 2u + 1u) ^ k)) + 0.5f) / 4294967296.0f;
    return sqrt(-2.0f * log(u1)) * cos(6.283185307179586f * u2);
}

struct RFParams { int hop; int overlap; int delay; int blocks; float scale; float signalLevel; float noiseSigma; int noiseKey; };

// initCodes: PPU codes -> composite voltage -> AM carrier (+ M3-NOISE), bit-reversed rows.
static inline float rfInitAt(int pos, device const ushort* codes, device const int* phases,
                            device const float* volts, constant RFParams& P) {
    float v = 0.312f;
    if (pos >= 0 && pos < 654720) {
        int row = pos / 2728; int n = pos % 2728; int phase = (n + phases[row]) % 12;
        if (n >= 36 && n < 2084) { int code = int(codes[row * 256 + (n - 36) / 8]); v = volts[phase * 512 + code]; }
        else if (n >= 2244 && n < 2444) v = 0.048f;
        else if (n >= 2476 && n < 2596) v = ((8 + phase) % 12 < 6) ? 0.524f : 0.148f;
    }
    float noise = (pos >= 0 && pos < 654720 && P.noiseSigma > 0.0f) ? P.noiseSigma * gaussAt(pos, P.noiseKey) : 0.0f;
    return (1.0f - P.scale * (v - 0.048f)) * P.signalLevel + noise;
}
static inline float rfInitValue(int x, int block, device const ushort* codes, device const int* phases,
                                device const float* volts, constant RFParams& P) {
    return rfInitAt(block * P.hop + P.delay - P.overlap + reverse12(x), codes, phases, volts, P);
}
kernel void rf_init_codes(device float2* outp [[buffer(0)]], device const ushort* codes [[buffer(1)]],
                          device const int* phases [[buffer(2)]], device const float* volts [[buffer(3)]],
                          constant RFParams& P [[buffer(4)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= 4096u || int(g.y) >= P.blocks) return;
    outp[g.y * 4096u + g.x] = float2(rfInitValue(int(g.x), int(g.y), codes, phases, volts, P), 0.0f);
}

struct FFTParams { int span; float signValue; int blocks; int pad; };
static inline float2 twv(device const float2* tw, int k, float s) { float2 p = tw[k]; return float2(p.x, s * p.y); }
static inline float2 stage1(device const float2* in, device const float2* tw, int k, int row, int span, float s) {
    int h = span / 2; int b = (k / span) * span; int j = k % h;
    float2 a = in[row * 4096 + b + j]; float2 c = in[row * 4096 + b + j + h];
    float2 t = cmul(c, twv(tw, j * (4096 / span), s));
    return (k % span < h) ? a + t : a - t;
}
// butterfly2: two consecutive radix-2 DIT stages (span, 2*span) in one pass.
kernel void rf_butterfly2(device const float2* in [[buffer(0)]], device float2* outp [[buffer(1)]],
                          device const float2* tw [[buffer(2)]], constant FFTParams& P [[buffer(3)]],
                          uint2 g [[thread_position_in_grid]]) {
    if (g.x >= 4096u || int(g.y) >= P.blocks) return;
    int x = int(g.x), row = int(g.y), span = P.span, s2 = span * 2;
    int base = (x / s2) * s2; int j = x % span;
    float2 a = stage1(in, tw, base + j, row, span, P.signValue);
    float2 c = stage1(in, tw, base + j + span, row, span, P.signValue);
    float2 t = cmul(c, twv(tw, j * (4096 / s2), P.signValue));
    outp[row * 4096 + x] = (x % s2 < span) ? a + t : a - t;
}
// butterfly: single radix-2 stage (unused for N=4096: every pass is butterfly2; kept for parity).
kernel void rf_butterfly(device const float2* in [[buffer(0)]], device float2* outp [[buffer(1)]],
                         device const float2* tw [[buffer(2)]], constant FFTParams& P [[buffer(3)]],
                         uint2 g [[thread_position_in_grid]]) {
    if (g.x >= 4096u || int(g.y) >= P.blocks) return;
    int x = int(g.x), row = int(g.y), span = P.span, halfSpan = span / 2;
    int j = x % halfSpan; int base = (x / span) * span;
    float2 a = in[row * 4096 + base + j]; float2 b = in[row * 4096 + base + j + halfSpan];
    float2 t = cmul(b, twv(tw, j * (4096 / span), P.signValue));
    outp[row * 4096 + x] = (x % span < halfSpan) ? a + t : a - t;
}
kernel void rf_multiply(device const float2* in [[buffer(0)]], device float2* outp [[buffer(1)]],
                        device const float2* kernelSpec [[buffer(2)]], constant FFTParams& P [[buffer(3)]],
                        uint2 g [[thread_position_in_grid]]) {
    if (g.x >= 4096u || int(g.y) >= P.blocks) return;
    int j = reverse12(int(g.x));
    outp[g.y * 4096u + g.x] = cmul(in[int(g.y) * 4096 + j], kernelSpec[j]);
}
kernel void rf_extract(device const float2* in [[buffer(0)]], device float2* carrier [[buffer(1)]],
                       constant RFParams& P [[buffer(2)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= 2728u || g.y >= 240u) return;
    int n = int(g.y) * 2728 + int(g.x);
    carrier[n] = in[(n / P.hop) * 4096 + n % P.hop + P.overlap] / 4096.0f;
}

// Same overlap-save FFT with every radix-2 DIT stage done in threadgroup memory (one 4096-point
// block per threadgroup). Each stage performs exactly the butterfly / butterfly2 arithmetic
// (a +- cmul(b, twiddle)), so the result equals the multi-pass port; it only avoids 12 round trips
// through device memory. Forward: init + FFT(-1). Inverse: kernel multiply (bit-reversed) +
// FFT(+1) + extract.
static inline void fftStagesTG(threadgroup float2* buf, device const float2* tw, float sgn, uint tid, uint nthreads) {
    for (int span = 2; span <= 4096; span *= 2) {
        int h = span / 2, step = 4096 / span;
        for (int p = int(tid); p < 2048; p += int(nthreads)) {
            int j = p % h, i0 = (p / h) * span + j, i1 = i0 + h;
            float2 a = buf[i0], t = cmul(buf[i1], twv(tw, j * step, sgn));
            buf[i0] = a + t; buf[i1] = a - t;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
}
kernel void rf_forward_tg(device float2* outp [[buffer(0)]], device const ushort* codes [[buffer(1)]],
                          device const int* phases [[buffer(2)]], device const float* volts [[buffer(3)]],
                          constant RFParams& P [[buffer(4)]], device const float2* tw [[buffer(5)]],
                          threadgroup float2* buf [[threadgroup(0)]],
                          uint tid [[thread_index_in_threadgroup]], uint nthreads [[threads_per_threadgroup]],
                          uint blk [[threadgroup_position_in_grid]]) {
    for (int i = int(tid); i < 4096; i += int(nthreads)) buf[i] = float2(rfInitValue(i, int(blk), codes, phases, volts, P), 0.0f);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    fftStagesTG(buf, tw, -1.0f, tid, nthreads);
    for (int i = int(tid); i < 4096; i += int(nthreads)) outp[blk * 4096u + uint(i)] = buf[i];
}
kernel void rf_inverse_tg(device const float2* spectrum [[buffer(0)]], device float2* carrier [[buffer(1)]],
                          device const float2* kernelSpec [[buffer(2)]], constant RFParams& P [[buffer(3)]],
                          device const float2* tw [[buffer(4)]], threadgroup float2* buf [[threadgroup(0)]],
                          uint tid [[thread_index_in_threadgroup]], uint nthreads [[threads_per_threadgroup]],
                          uint blk [[threadgroup_position_in_grid]]) {
    for (int i = int(tid); i < 4096; i += int(nthreads)) { int j = reverse12(i); buf[i] = cmul(spectrum[blk * 4096u + uint(j)], kernelSpec[j]); }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    fftStagesTG(buf, tw, 1.0f, tid, nthreads);
    for (int k = int(tid); k < P.hop; k += int(nthreads)) {
        int n = int(blk) * P.hop + k;
        if (n < 654720) carrier[n] = buf[k + P.overlap] / 4096.0f;
    }
}

// ---------------------------------------------------------------- receiver-webgl.mjs
// reduction: porch clamp, burst correlation, sync level per row.
kernel void rx_reduce(device const float2* carrier [[buffer(0)]], device const float2* basis [[buffer(1)]],
                      device float4* stats [[buffer(2)]], uint g [[thread_position_in_grid]]) {
    if (g >= 240u) return;
    int y = int(g); float porch = 0.0f, bc = 0.0f, bs = 0.0f, sync = 0.0f;
    for (int n = 2172; n < 2244; n++) porch += carrier[y * 2728 + n].x;
    for (int n = 2476; n < 2596; n++) { float a = carrier[y * 2728 + n].x; float2 b = basis[n]; bc += a * b.x; bs += a * b.y; }
    for (int n = 2324; n < 2364; n++) sync += carrier[y * 2728 + n].x;
    stats[y] = float4(porch / 72.0f, bc / 60.0f, bs / 60.0f, sync / 40.0f);
}

// GatedAGC (reference/agc.mjs) over the 240 rows, on the GPU instead of the JS readback loop.
struct AGCParams { float lineSeconds; float attack; float release; float minGain; float maxGain; int enabled; int delay; int pad; };
struct AGCState { float gain; float signalLost; float pad0; float pad1; };
kernel void rx_agc(device const float4* stats [[buffer(0)]], device float* gains [[buffer(1)]],
                   device AGCState* state [[buffer(2)]], constant AGCParams& P [[buffer(3)]],
                   uint g [[thread_position_in_grid]]) {
    if (g != 0u) return;
    float lo = log(P.minGain), hi = log(P.maxGain);
    float gain = state->gain;
    float logGain = log(gain);   // a fresh GatedAGC({initialGain: controller.gain}) per packet
    float lost = 0.0f;
    for (int row = 0; row < 240; row++) {
        float gainBefore = gain;
        float4 s = stats[row];
        bool gateValid = row * 2728 + 2324 - P.delay >= 0 && row * 2728 + 2363 + P.delay < 654720;
        float detectedSync = gainBefore * s.w;
        float amplitude = gainBefore * sqrt(s.y * s.y + s.z * s.z) / (0.875f / (1.100f - 0.048f));
        if (amplitude < 1e-12f) lost = 1.0f;
        gains[row] = gainBefore;
        bool controlGate = P.enabled != 0 && gateValid && detectedSync > 0.0f;
        if (controlGate) {
            float desired = clamp(logGain - log(detectedSync), lo, hi);
            float tau = desired < logGain ? P.attack : P.release;
            float fraction = 1.0f - exp(-P.lineSeconds / tau);   // -expm1(-dt/tau)
            logGain = clamp(logGain + (desired - logGain) * fraction, lo, hi);
            gain = clamp(exp(logGain), P.minGain, P.maxGain);
        }
    }
    state->gain = gain;
    state->signalLost = lost;
}

kernel void rx_prepare(device const float2* carrier [[buffer(0)]], device const float2* basis [[buffer(1)]],
                       device const float4* stats [[buffer(2)]], device float4* prepared [[buffer(3)]],
                       uint2 g [[thread_position_in_grid]]) {
    if (g.x >= 682u || g.y >= 240u) return;
    int px = int(g.x), py = int(g.y);
    float porch = stats[py].x;
    float Y = 0.0f, C = 0.0f, S = 0.0f;
    for (int k = 0; k < 4; k++) {
        int n = px * 4 + k; Y += porch - carrier[py * 2728 + n].x;
        n = min(n + 2, 2727); float a = porch - carrier[py * 2728 + n].x;
        float2 b = basis[n]; C += a * b.x; S += a * b.y;
    }
    prepared[py * 682 + px] = float4(Y, C, S, 0.0f);
}

kernel void rx_decode(device const float4* prepared [[buffer(0)]], device const float4* stats [[buffer(1)]],
                      device const float* gains [[buffer(2)]], device float4* outp [[buffer(3)]],
                      uint2 g [[thread_position_in_grid]]) {
    if (g.x >= 512u || g.y >= 240u) return;
    int px = int(g.x), py = int(g.y);
    float4 stat = stats[py]; float gn = gains[py];
    float lengthC = length(stat.yz); float2 rot = float2(-stat.y, stat.z) / max(lengthC, 1e-30f);
    float Y = 0.0f; float2 CS = float2(0.0f);
    for (int k = 0; k < 3; k++) Y += prepared[py * 682 + px + 8 + k].x;
    for (int k = 0; k < 12; k++) CS += prepared[py * 682 + px + 3 + k].yz;
    float factor = gn / ((0.875f / (1.100f - 0.048f)) * (1.100f - 0.312f));
    Y *= factor / 12.0f;
    float u = (CS.x * rot.x - CS.y * rot.y) * factor * (-2.0f / 48.0f);
    float cv = (CS.y * rot.x + CS.x * rot.y) * factor * (2.0f / 48.0f);
    float I = 0.838670568f * cv - 0.544639035f * u, Q = 0.544639035f * cv + 0.838670568f * u;
    float3 rgb = float3(Y + 0.956f * I + 0.621f * Q, Y - 0.272f * I - 0.647f * Q, Y - 1.106f * I + 1.703f * Q);
    outp[py * 512 + px] = float4(pow(clamp(rgb, 0.0f, 1.0f), float3(2.2f)), 1.0f);
}

// ---------------------------------------------------------------- raster-webgl.mjs
static inline float3 linearize(float3 v) {
    float3 a = pow((v + 0.055f) / 1.055f, float3(2.4f)), b = v / 12.92f;
    return select(a, b, v <= float3(0.04045f));
}
struct RasterParams { int lines; int decode; int rgbSource; int pad; };
// Area integration of 240 source rows into `lines` rows in linear drive (COMPOSE-RASTER).
// rgbSource: the 512x240 synthetic RGB image is ReplayNES's 256x240 BGRA8 frame with each pixel
// doubled horizontally (COMPOSE-ASCII-RGB input; unorm8 -> c/255 like a WebGL RGBA8 texture).
kernel void raster_area(device const float4* src [[buffer(0)]], device const uint* bgra [[buffer(1)]],
                        device float4* outp [[buffer(2)]], constant RasterParams& P [[buffer(3)]],
                        uint2 g [[thread_position_in_grid]]) {
    if (g.x >= 512u || int(g.y) >= P.lines) return;
    int px = int(g.x), py = int(g.y);
    int low = py * 240, high = (py + 1) * 240;
    float3 sum = float3(0.0f); int first = low / P.lines;
    for (int k = 0; k < 4; k++) {
        int y = first + k; if (y * P.lines >= high) break;
        float3 v;
        if (P.rgbSource != 0) { uint c = bgra[y * 256 + px / 2]; v = float3(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u)) / 255.0f; }
        else v = src[y * 512 + px].rgb;
        if (P.decode != 0) v = linearize(v);
        sum += v * float(min(high, (y + 1) * P.lines) - max(low, y * P.lines));
    }
    outp[py * 512 + px] = float4(sum / 240.0f, 1.0f);
}

// ---------------------------------------------------------------- supply-webgl.mjs (M4c)
struct SupplyParams {
    int width; int rows; float decay; float v0; float reff; float imax; float n; float relax;
    float reqScale; float ablFraction; float ilim; float pad; float4 share;
};
kernel void supply_mean(device const float4* src [[buffer(0)]], device float4* means [[buffer(1)]],
                        constant SupplyParams& P [[buffer(2)]], uint g [[thread_position_in_grid]]) {
    if (int(g) >= P.rows) return;
    int y = int(g); float3 s = float3(0.0f);
    for (int x = 0; x < P.width; x++) s += src[y * P.width + x].rgb;
    means[y] = float4(s / float(P.width), 1.0f);
}
// Shared recurrence over rows 0..last with the gain in force (state.b).
static inline float supplyLoop(device const float4* means, float4 state, constant SupplyParams& P, int last, thread float& req) {
    float v = state.x, gain = state.z; req = 0.0f;
    for (int j = 0; j < P.rows; j++) {
        if (j > last) break;
        float3 u = means[j].rgb;
        float drive = dot(u, P.share.xyz);
        float target = P.v0 - P.reff * P.imax * gain * drive;
        v = target + (v - target) * P.decay; req += P.imax * drive;
    }
    return v;
}
// r: magnification, g: gain*brightness/m (deposited light per length).
kernel void supply_row(device const float4* means [[buffer(0)]], device const float4* state [[buffer(1)]],
                       device float4* perRow [[buffer(2)]], constant SupplyParams& P [[buffer(3)]],
                       uint g [[thread_position_in_grid]]) {
    if (int(g) >= P.rows) return;
    float4 st = state[0]; float req;
    float v = supplyLoop(means, st, P, int(g), req);
    float m = sqrt(P.v0 / v);
    perRow[g] = float4(m, st.z * pow(v / P.v0, P.n) / m, 0.0f, 1.0f);
}
// Next frame's state: relax over blanking, smooth the requested current, new ABL gain.
// P.pad != 0 (ReplayNES, start of history: first frame, or after a seek / rewind / load): the
// steady state of this picture held on screen instead - the requested current settled to this
// frame's (req does not depend on v or the gain) and the anode voltage iterated to its fixed point
// (contracts by relax * decay^rows per pass, ~1e-5) - so a still is shown as in continuous play.
kernel void supply_state(device const float4* means [[buffer(0)]], device const float4* state [[buffer(1)]],
                         device float4* next [[buffer(2)]], constant SupplyParams& P [[buffer(3)]],
                         uint g [[thread_position_in_grid]]) {
    if (g != 0u) return;
    if (P.pad != 0.0f) {
        float req;
        supplyLoop(means, float4(P.v0, 0.0f, 1.0f, 1.0f), P, P.rows - 1, req);
        float requested = req * P.reqScale, gain = requested > P.ilim ? P.ilim / requested : 1.0f, v = P.v0;
        for (int i = 0; i < 3; i++) { float r2; v = supplyLoop(means, float4(v, requested, gain, 1.0f), P, P.rows - 1, r2); v = P.v0 + (v - P.v0) * P.relax; }
        next[0] = float4(v, requested, gain, 1.0f);
        return;
    }
    float4 st = state[0]; float req;
    float v = supplyLoop(means, st, P, P.rows - 1, req);
    v = P.v0 + (v - P.v0) * P.relax;
    float requested = st.y + (req * P.reqScale - st.y) * P.ablFraction;
    next[0] = float4(v, requested, requested > P.ilim ? P.ilim / requested : 1.0f, 1.0f);
}
kernel void supply_resample(device const float4* src [[buffer(0)]], device const float4* perRow [[buffer(1)]],
                            device float4* outp [[buffer(2)]], constant SupplyParams& P [[buffer(3)]],
                            uint2 g [[thread_position_in_grid]]) {
    if (int(g.x) >= P.width || int(g.y) >= P.rows) return;
    int px = int(g.x), py = int(g.y);
    float2 k = perRow[py].xy;
    float halfW = float(P.width) / 2.0f;
    float sx = halfW + (float(px) + 0.5f - halfW) / k.x - 0.5f; int x0 = int(floor(sx)); float t = sx - float(x0);
    float3 a = (x0 >= 0 && x0 < P.width) ? src[py * P.width + x0].rgb : float3(0.0f);
    float3 b = (x0 + 1 >= 0 && x0 + 1 < P.width) ? src[py * P.width + x0 + 1].rgb : float3(0.0f);
    outp[py * P.width + px] = float4(glmix3(a, b, t) * k.y, 1.0f);
}

// ---------------------------------------------------------------- spot-h-webgl.mjs (M4C-SPOT-H)
struct SpotParams { int width; int rows; int radius; float k1; };
static inline float spotW(float u, int k, constant SpotParams& P) {
    float s = P.k1 * sqrt(max(u, 0.0f));
    if (s <= 1e-6f) return k == 0 ? 1.0f : 0.0f;
    float sum = 0.0f;
    for (int i = -P.radius; i <= P.radius; i++) sum += exp(-0.5f * float(i * i) / (s * s));
    return exp(-0.5f * float(k * k) / (s * s)) / sum;
}
kernel void spot_h(device const float4* src [[buffer(0)]], device float4* outp [[buffer(1)]],
                   constant SpotParams& P [[buffer(2)]], uint2 g [[thread_position_in_grid]]) {
    if (int(g.x) >= P.width || int(g.y) >= P.rows) return;
    int px = int(g.x), py = int(g.y);
    float3 acc = float3(0.0f);
    for (int k = -P.radius; k <= P.radius; k++) {
        int j = px - k; if (j < 0 || j >= P.width) continue;
        float3 u = src[py * P.width + j].rgb;
        acc += float3(u.r * spotW(u.r, k, P), u.g * spotW(u.g, k, P), u.b * spotW(u.b, k, P));
    }
    outp[py * P.width + px] = float4(acc, 1.0f);
}

// ---------------------------------------------------------------- tube-webgl.mjs (M1 / M4a / M4b)
struct TubeParams {
    int width; int height; int ow; int oh; int xCount; int yCount; int gCount; int levels;
    float gain; float scatterFraction; float keep; int radius; int depth; int pad0; int pad1; int pad2;
};
// hprog: horizontal detector integration per (column, branch c*2+parity), rows height*2.
kernel void tube_h(device const float4* src [[buffer(0)]], device const float2* xmap [[buffer(1)]],
                   device float4* outp [[buffer(2)]], constant TubeParams& P [[buffer(3)]],
                   uint2 g [[thread_position_in_grid]]) {
    if (int(g.x) >= P.ow || int(g.y) >= P.height * 2) return;
    int px = int(g.x); int parity = int(g.y) / P.height; int y = int(g.y) % P.height;
    float3 sum = float3(0.0f);
    for (int c = 0; c < 3; c++) for (int k = 0; k < P.xCount; k++) {
        float2 w = xmap[(px * 6 + c * 2 + parity) * P.xCount + k];
        sum[c] += src[y * P.width + int(w.x)][c] * w.y;
    }
    outp[int(g.y) * P.ow + px] = float4(sum, 1.0f);
}
// vprog (fixed spot).
kernel void tube_v(device const float4* hsrc [[buffer(0)]], device const float2* ymap [[buffer(1)]],
                   device float4* outp [[buffer(2)]], constant TubeParams& P [[buffer(3)]],
                   uint2 g [[thread_position_in_grid]]) {
    if (int(g.x) >= P.ow || int(g.y) >= P.oh) return;
    int px = int(g.x), py = int(g.y);
    float3 value = float3(0.0f);
    for (int parity = 0; parity < 2; parity++) {
        float3 sum = float3(0.0f);
        for (int k = 0; k < P.yCount; k++) {
            float2 w = ymap[(py * 2 + parity) * P.yCount + k];
            sum += hsrc[(int(w.x) + parity * P.height) * P.ow + px].rgb * w.y;
        }
        value += sum * P.gain;
    }
    outp[py * P.ow + px] = float4(value, 1.0f);
}
// vprog (M4B-GPU-CURRENT): drive-dependent vertical spot, tables interpolated in current.
kernel void tube_v_growth(device const float4* hsrc [[buffer(0)]], device const float2* gmap [[buffer(1)]],
                          device const float* colsum [[buffer(2)]], device float4* outp [[buffer(3)]],
                          constant TubeParams& P [[buffer(4)]], uint2 g [[thread_position_in_grid]]) {
    if (int(g.x) >= P.ow || int(g.y) >= P.oh) return;
    int px = int(g.x), py = int(g.y);
    int rowWidth = P.levels * P.gCount;
    float3 value = float3(0.0f);
    for (int parity = 0; parity < 2; parity++) {
        float3 cs = float3(colsum[parity * P.ow + px], colsum[(2 + parity) * P.ow + px], colsum[(4 + parity) * P.ow + px]);
        float3 sum = float3(0.0f); int row = py * 2 + parity;
        for (int k = 0; k < P.gCount; k++) {
            float fy = gmap[row * rowWidth + k].x; if (fy < 0.0f) continue;
            float3 h = hsrc[(int(fy) + parity * P.height) * P.ow + px].rgb;
            for (int c = 0; c < 3; c++) {
                if (h[c] == 0.0f || cs[c] <= 0.0f) continue;
                float t = clamp(h[c] / cs[c], 0.0f, 1.0f) * float(P.levels - 1);
                int kk = min(P.levels - 2, int(floor(t))); float f = t - float(kk);
                sum[c] += h[c] * glmix1(gmap[row * rowWidth + kk * P.gCount + k].y, gmap[row * rowWidth + (kk + 1) * P.gCount + k].y, f);
            }
        }
        value += sum * P.gain;
    }
    outp[py * P.ow + px] = float4(value, 1.0f);
}
// sprogs[axis]: finite-screen scatter, zero extension.
kernel void tube_scatter(device const float4* src [[buffer(0)]], device const float* weights [[buffer(1)]],
                         device float4* outp [[buffer(2)]], constant TubeParams& P [[buffer(3)]],
                         constant int& axis [[buffer(4)]], uint2 g [[thread_position_in_grid]]) {
    if (int(g.x) >= P.ow || int(g.y) >= P.oh) return;
    int px = int(g.x), py = int(g.y);
    float3 sum = float3(0.0f);
    for (int d = -P.radius; d <= P.radius; d++) {
        int qx = px + (axis != 0 ? 0 : d), qy = py + (axis != 0 ? d : 0);
        if (qx >= 0 && qy >= 0 && qx < P.ow && qy < P.oh) sum += src[qy * P.ow + qx].rgb * weights[d + P.radius];
    }
    outp[py * P.ow + px] = float4(sum, 1.0f);
}
// Same arithmetic as tube_scatter: every output sums its taps d = -R..R in the same order with
// the same weights (out-of-screen taps add an exact +0). Restructured for speed because the
// 2 mm scatter spans ~90 taps at 1600 px: each thread makes 16 consecutive outputs along the
// axis from a sliding register window (one source read per tap step), weights in constant space.
template <int K>
static inline void scatterSlide(device const float4* src, constant float* weights, device float4* outp,
                                constant TubeParams& P, int px, int py, int ax, int ay) {
    int R = P.radius;
    float3 win[K], sum[K];
    for (int k = 0; k < K; k++) {
        int qx = px + ax * (k - R), qy = py + ay * (k - R);
        win[k] = (qx >= 0 && qy >= 0 && qx < P.ow && qy < P.oh) ? src[qy * P.ow + qx].rgb : float3(0.0f);
        sum[k] = float3(0.0f);
    }
    for (int d = -R; d <= R; d++) {
        float w = weights[d + R];
        for (int k = 0; k < K; k++) sum[k] += win[k] * w;
        for (int k = 0; k < K - 1; k++) win[k] = win[k + 1];
        int qx = px + ax * (K + d), qy = py + ay * (K + d);
        win[K - 1] = (qx >= 0 && qy >= 0 && qx < P.ow && qy < P.oh) ? src[qy * P.ow + qx].rgb : float3(0.0f);
    }
    for (int k = 0; k < K; k++) {
        int qx = px + ax * k, qy = py + ay * k;
        if (qx < P.ow && qy < P.oh) outp[qy * P.ow + qx] = float4(sum[k], 1.0f);
    }
}
kernel void tube_scatter_x16(device const float4* src [[buffer(0)]], constant float* weights [[buffer(1)]],
                             device float4* outp [[buffer(2)]], constant TubeParams& P [[buffer(3)]],
                             uint2 g [[thread_position_in_grid]]) {
    if (int(g.x) * 16 >= P.ow || int(g.y) >= P.oh) return;
    scatterSlide<16>(src, weights, outp, P, int(g.x) * 16, int(g.y), 1, 0);
}
kernel void tube_scatter_y16(device const float4* src [[buffer(0)]], constant float* weights [[buffer(1)]],
                             device float4* outp [[buffer(2)]], constant TubeParams& P [[buffer(3)]],
                             uint2 g [[thread_position_in_grid]]) {
    if (int(g.x) >= P.ow || int(g.y) * 16 >= P.oh) return;
    scatterSlide<16>(src, weights, outp, P, int(g.x), int(g.y) * 16, 0, 1);
}
// mixprog (no persistence): ambient + emitted light.
kernel void tube_mix(device const float4* emission [[buffer(0)]], device const float4* broad [[buffer(1)]],
                     device const float* ambient [[buffer(2)]], device float4* outp [[buffer(3)]],
                     constant TubeParams& P [[buffer(4)]], uint2 g [[thread_position_in_grid]]) {
    if (int(g.x) >= P.ow || int(g.y) >= P.oh) return;
    int i = int(g.y) * P.ow + int(g.x);
    outp[i] = float4(float3(ambient[i]) + emission[i].rgb * P.keep + broad[i].rgb * P.scatterFraction, 1.0f);
}
// litprog: emitted light (after scatter, before ambient) into a half-float ring slot.
kernel void tube_lit(device const float4* emission [[buffer(0)]], device const float4* broad [[buffer(1)]],
                     device half4* ring [[buffer(2)]], constant TubeParams& P [[buffer(3)]],
                     constant int& slot [[buffer(4)]], uint2 g [[thread_position_in_grid]]) {
    if (int(g.x) >= P.ow || int(g.y) >= P.oh) return;
    int i = int(g.y) * P.ow + int(g.x);
    ring[slot * P.ow * P.oh + i] = half4(float4(emission[i].rgb * P.keep + broad[i].rgb * P.scatterFraction, 1.0f));
}
// persistprog: ambient + sum of ring slots weighted by the phosphor frame fractions.
kernel void tube_persist(device const half4* ring [[buffer(0)]], device const float* ambient [[buffer(1)]],
                         device float4* outp [[buffer(2)]], constant TubeParams& P [[buffer(3)]],
                         constant float4* w [[buffer(4)]], uint2 g [[thread_position_in_grid]]) {
    if (int(g.x) >= P.ow || int(g.y) >= P.oh) return;
    int i = int(g.y) * P.ow + int(g.x);
    float3 v = float3(ambient[i]);
    for (int s = 0; s < P.depth; s++) v += float3(ring[s * P.ow * P.oh + i].rgb) * w[s].xyz;
    outp[i] = float4(v, 1.0f);
}

// showprog: clamp >= 0, sRGB encode, min 1. Metal targets are top-left origin, so the GL Y flip
// is not needed. The destination rectangle samples the tube output in linear light (1:1 when the
// tube is rendered at the destination size; bilinear otherwise).
struct ShowParams { float4 dst; float4 src; int ow; int oh; int pad0; int pad1; };
struct ShowOut { float4 pos [[position]]; };
vertex ShowOut show_vertex(uint vid [[vertex_id]], constant float4& ndc [[buffer(0)]]) {
    float2 c = float2(float(vid & 1u), float(vid >> 1));
    ShowOut o; o.pos = float4(ndc.x + c.x * ndc.z, ndc.y + c.y * ndc.w, 0.0f, 1.0f); return o;
}
static inline float3 tubeAt(device const float4* img, int x, int y, int ow, int oh) {
    x = clamp(x, 0, ow - 1); y = clamp(y, 0, oh - 1); return img[y * ow + x].rgb;
}
static inline float3 encodeSRGB(float3 v) {
    v = max(float3(0.0f), v);
    float3 s = select(1.055f * pow(v, float3(1.0f / 2.4f)) - 0.055f, 12.92f * v, v <= float3(0.0031308f));
    return min(s, float3(1.0f));
}
static inline float3 showSample(device const float4* img, constant ShowParams& P, float2 frag) {
    float u = (frag.x - P.dst.x) / P.dst.z * P.src.z + P.src.x - 0.5f;
    float v = (frag.y - P.dst.y) / P.dst.w * P.src.w + P.src.y - 0.5f;
    float x0 = floor(u), y0 = floor(v), fx = u - x0, fy = v - y0;
    int ix = int(x0), iy = int(y0);
    float3 a = tubeAt(img, ix, iy, P.ow, P.oh), b = tubeAt(img, ix + 1, iy, P.ow, P.oh);
    float3 c = tubeAt(img, ix, iy + 1, P.ow, P.oh), d = tubeAt(img, ix + 1, iy + 1, P.ow, P.oh);
    float3 top = fx == 0.0f ? a : a * (1.0f - fx) + b * fx;
    float3 bot = fx == 0.0f ? c : c * (1.0f - fx) + d * fx;
    return fy == 0.0f ? top : top * (1.0f - fy) + bot * fy;
}
fragment float4 show_fragment(ShowOut in [[stage_in]], device const float4* img [[buffer(0)]], constant ShowParams& P [[buffer(1)]]) {
    return float4(encodeSRGB(showSample(img, P, in.pos.xy)), 1.0f);
}
// Same as show_fragment into a compute target (offline export / tests): dst = target pixels.
kernel void show_kernel(device const float4* img [[buffer(0)]], constant ShowParams& P [[buffer(1)]],
                        texture2d<float, access::write> target [[texture(0)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= target.get_width() || g.y >= target.get_height()) return;
    float2 frag = float2(g) + 0.5f;
    bool inside = frag.x >= P.dst.x && frag.y >= P.dst.y && frag.x < P.dst.x + P.dst.z && frag.y < P.dst.y + P.dst.w;
    target.write(inside ? float4(encodeSRGB(showSample(img, P, frag)), 1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f), g);
}
"""#

    /// Fast path (CRTRenderer.Quality.fast, the live display): the same model and stage order with
    /// the work restructured and a few approximations far below 8-bit display precision. Compiled
    /// together with `reference` (shares its helpers and parameter structs). docs/CRT_PORT.md
    /// "Fast path" lists every difference; CRTTests bounds the error against the reference.
    static let fast = #"""
// ================================================================ fast path
static inline float2 cmulF(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
// 8-point DFT in registers: X[k] = sum_n v[n] e^(s i 2 pi n k / 8), natural order in and out.
static inline void dft8(thread float2* v, float s) {
    const float h = 0.70710678118654752f;
    float2 a0 = v[0] + v[4], a1 = v[1] + v[5], a2 = v[2] + v[6], a3 = v[3] + v[7];
    float2 b0 = v[0] - v[4], b1 = v[1] - v[5], b2 = v[2] - v[6], b3 = v[3] - v[7];
    b1 = float2(h * (b1.x - s * b1.y), h * (b1.y + s * b1.x));
    b2 = float2(-s * b2.y, s * b2.x);
    b3 = float2(-h * (b3.x + s * b3.y), h * (s * b3.x - b3.y));
    float2 c0 = a0 + a2, c1 = a1 + a3, d0 = a0 - a2, d1 = a1 - a3; d1 = float2(-s * d1.y, s * d1.x);
    float2 e0 = b0 + b2, e1 = b1 + b3, f0 = b0 - b2, f1 = b1 - b3; f1 = float2(-s * f1.y, s * f1.x);
    v[0] = c0 + c1; v[4] = c0 - c1; v[2] = d0 + d1; v[6] = d0 - d1;
    v[1] = e0 + e1; v[5] = e0 - e1; v[3] = f0 + f1; v[7] = f0 - f1;
}
// 4096-point Stockham radix-8 FFT, 512 threads (j), natural order in and out. The data stay in
// registers (8 points per thread); each pass exchanges them through 16 KB of threadgroup memory,
// real and imaginary parts in turn (half the memory of a complex buffer: two threadgroups fit a
// core). Twiddles from the same float32 table as the reference butterflies.
static inline void fftPass(thread float2* v, threadgroup float* x, device const float2* tw, float s, int j, int ns) {
    int k = j % ns, step = 4096 / (ns * 8);
    if (k > 0) {
        // w^r, w = e^(s i 2 pi k step / 4096) from the twiddle table, powers by multiplication
        // (one table read per pass instead of seven).
        float2 t = tw[k * step], w1 = float2(t.x, s * t.y), wr = w1;
        for (int r = 1; r < 8; r++) { v[r] = cmulF(v[r], wr); wr = cmulF(wr, w1); }
    }
    dft8(v, s);
    int d = (j / ns) * ns * 8 + k;
    int nsNext = ns * 8;
    if (nsNext >= 4096) {   // last pass: leave the natural-order result in x as (re, im) planes
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (int r = 0; r < 8; r++) x[d + r * ns] = v[r].x;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        float re[8];
        for (int r = 0; r < 8; r++) re[r] = x[j + r * 512];
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (int r = 0; r < 8; r++) x[d + r * ns] = v[r].y;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (int r = 0; r < 8; r++) v[r] = float2(re[r], x[j + r * 512]);
        return;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int r = 0; r < 8; r++) x[d + r * ns] = v[r].x;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float re[8];
    for (int r = 0; r < 8; r++) re[r] = x[j + r * 512];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int r = 0; r < 8; r++) x[d + r * ns] = v[r].y;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int r = 0; r < 8; r++) v[r] = float2(re[r], x[j + r * 512]);
}
// v[r] = element j + 512 r on entry and on exit.
static inline void fft4096(thread float2* v, threadgroup float* x, device const float2* tw, float s, int j) {
    for (int ns = 1; ns < 4096; ns *= 8) fftPass(v, x, tw, s, j, ns);
}
// RF/IF stage: the receiver only uses the real part of the IF output, i.e. the composite signal
// filtered by Re(h) (a real FIR). Two consecutive overlap-save blocks ride in one complex FFT
// (block 2m real, 2m+1 imaginary), multiplied by the spectrum of Re(h) and transformed back in the
// same threadgroup: real part = block 2m, imaginary part = block 2m+1. Output: carrier (real).
kernel void rf_fast(device float* carrier [[buffer(0)]], device const ushort* codes [[buffer(1)]],
                    device const int* phases [[buffer(2)]], device const float* volts [[buffer(3)]],
                    constant RFParams& P [[buffer(4)]], device const float2* tw [[buffer(5)]],
                    device const float2* hspec [[buffer(6)]], threadgroup float* x [[threadgroup(0)]],
                    uint tid [[thread_index_in_threadgroup]], uint pair [[threadgroup_position_in_grid]]) {
    int b0 = int(pair) * 2, j = int(tid);
    bool has1 = b0 + 1 < P.blocks;
    int base0 = b0 * P.hop + P.delay - P.overlap, base1 = base0 + P.hop;
    float2 v[8];
    for (int r = 0; r < 8; r++) {
        int i = j + r * 512;
        v[r] = float2(rfInitAt(base0 + i, codes, phases, volts, P), has1 ? rfInitAt(base1 + i, codes, phases, volts, P) : 0.0f);
    }
    fft4096(v, x, tw, -1.0f, j);
    for (int r = 0; r < 8; r++) v[r] = cmulF(v[r], hspec[j + r * 512]);
    fft4096(v, x, tw, 1.0f, j);
    for (int r = 0; r < 8; r++) {
        int k = j + r * 512 - P.overlap;
        if (k < 0 || k >= P.hop) continue;
        float2 y = v[r] * (1.0f / 4096.0f);
        int n0 = b0 * P.hop + k, n1 = n0 + P.hop;
        if (n0 < 654720) carrier[n0] = y.x;
        if (has1 && n1 < 654720) carrier[n1] = y.y;
    }
}
// receiver-webgl reduction of one row (64 threads): porch clamp, burst correlation, sync level;
// plus the row's GatedAGC control target. log(gain * sync) = log(gain) + log(sync), so the target
// clamp(log(gain) - log(gain * sync)) is clamp(-log(sync)), independent of the gain in force:
// agc[y] = (target, control gate on, 0, 0).
kernel void rx_stats_fast(device const float* carrier [[buffer(0)]], device const float2* basis [[buffer(1)]],
                          device float4* stats [[buffer(2)]], device float4* agc [[buffer(3)]],
                          constant AGCParams& P [[buffer(4)]],
                          uint tid [[thread_index_in_threadgroup]], uint row [[threadgroup_position_in_grid]]) {
    threadgroup float4 part[64];
    int y = int(row), t = int(tid);
    device const float* c = carrier + y * 2728;
    float porch = 0.0f, bc = 0.0f, bs = 0.0f, sync = 0.0f;
    for (int n = 2172 + t; n < 2244; n += 64) porch += c[n];
    for (int n = 2476 + t; n < 2596; n += 64) { float a = c[n]; float2 b = basis[n]; bc += a * b.x; bs += a * b.y; }
    for (int n = 2324 + t; n < 2364; n += 64) sync += c[n];
    part[t] = float4(porch, bc, bs, sync);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int w = 32; w > 0; w >>= 1) {
        if (t < w) part[t] += part[t + w];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (t == 0) {
        float4 s = part[0] / float4(72.0f, 60.0f, 60.0f, 40.0f);
        stats[y] = s;
        bool gateValid = y * 2728 + 2324 - P.delay >= 0 && y * 2728 + 2363 + P.delay < 654720;
        bool control = P.enabled != 0 && gateValid && s.w > 0.0f;
        agc[y] = float4(control ? clamp(-log(s.w), log(P.minGain), log(P.maxGain)) : 0.0f, control ? 1.0f : 0.0f, 0.0f, 0.0f);
    }
}
// GatedAGC over the 240 rows from the per-row control targets (one threadgroup: thread 0 runs the
// recurrence, a few multiply-adds per row; the gains are exponentiated in parallel). The control
// targets keep lg within [log minGain, log maxGain], so the reference's clamps are no-ops there.
kernel void rx_agc_fast(device const float4* agc [[buffer(0)]], device float* gains [[buffer(1)]],
                        device AGCState* state [[buffer(2)]], constant AGCParams& P [[buffer(3)]],
                        uint tid [[thread_index_in_threadgroup]]) {
    threadgroup float2 target[240];
    threadgroup float logg[240];
    threadgroup bool changedT;
    threadgroup float g0T;
    int t = int(tid);
    if (t < 240) target[t] = agc[t].xy;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (t == 0) {
        float lo = log(P.minGain), hi = log(P.maxGain);
        float fa = 1.0f - exp(-P.lineSeconds / P.attack), fr = 1.0f - exp(-P.lineSeconds / P.release);
        float g0 = state->gain, lg = log(g0);
        bool changed = false;
        for (int r = 0; r < 240; r++) {
            logg[r] = changed ? lg : 1e30f;
            float2 q = target[r];
            if (q.y != 0.0f) { lg = clamp(lg + (q.x - lg) * (q.x < lg ? fa : fr), lo, hi); changed = true; }
        }
        if (changed) state->gain = clamp(exp(lg), P.minGain, P.maxGain);
        state->signalLost = 0.0f;
        g0T = g0;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (t < 240) gains[t] = logg[t] > 1e29f ? g0T : clamp(exp(logg[t]), P.minGain, P.maxGain);
}
// prepare + decode of one row (256 threads); the 682 prepared (Y, C, S) values stay in
// threadgroup memory. Also the row's mean drive (supply_mean).
kernel void rx_decode_fast(device const float* carrier [[buffer(0)]], device const float2* basis [[buffer(1)]],
                           device const float4* stats [[buffer(2)]], device const float* gains [[buffer(3)]],
                           device float4* outp [[buffer(4)]], device float4* means [[buffer(5)]],
                           uint tid [[thread_index_in_threadgroup]], uint row [[threadgroup_position_in_grid]]) {
    threadgroup float3 prep[682];
    threadgroup float3 part[256];
    int py = int(row), t = int(tid);
    float4 stat = stats[py];
    float porch = stat.x;
    device const float* c = carrier + py * 2728;
    for (int px = t; px < 682; px += 256) {
        float Y = 0.0f, C = 0.0f, S = 0.0f;
        for (int k = 0; k < 4; k++) {
            int n = px * 4 + k; Y += porch - c[n];
            n = min(n + 2, 2727); float a = porch - c[n];
            float2 b = basis[n]; C += a * b.x; S += a * b.y;
        }
        prep[px] = float3(Y, C, S);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float gn = gains[py];
    float lengthC = length(stat.yz); float2 rot = float2(-stat.y, stat.z) / max(lengthC, 1e-30f);
    float factor = gn / ((0.875f / (1.100f - 0.048f)) * (1.100f - 0.312f));
    float3 total = float3(0.0f);
    for (int px = t; px < 512; px += 256) {
        float Y = 0.0f; float2 CS = float2(0.0f);
        for (int k = 0; k < 3; k++) Y += prep[px + 8 + k].x;
        for (int k = 0; k < 12; k++) CS += prep[px + 3 + k].yz;
        Y *= factor / 12.0f;
        float u = (CS.x * rot.x - CS.y * rot.y) * factor * (-2.0f / 48.0f);
        float cv = (CS.y * rot.x + CS.x * rot.y) * factor * (2.0f / 48.0f);
        float I = 0.838670568f * cv - 0.544639035f * u, Q = 0.544639035f * cv + 0.838670568f * u;
        float3 rgb = float3(Y + 0.956f * I + 0.621f * Q, Y - 0.272f * I - 0.647f * Q, Y - 1.106f * I + 1.703f * Q);
        rgb = pow(clamp(rgb, 0.0f, 1.0f), float3(2.2f));
        outp[py * 512 + px] = float4(rgb, 1.0f);
        total += rgb;
    }
    part[t] = total;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int w = 128; w > 0; w >>= 1) {
        if (t < w) part[t] += part[t + w];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (t == 0) means[py] = float4(part[0] / 512.0f, 1.0f);
}
// supply_mean as a tree reduction (128 threads per row).
kernel void row_mean_fast(device const float4* src [[buffer(0)]], device float4* means [[buffer(1)]],
                          uint tid [[thread_index_in_threadgroup]], uint row [[threadgroup_position_in_grid]]) {
    threadgroup float3 part[128];
    float3 s = float3(0.0f);
    for (int x = int(tid); x < 512; x += 128) s += src[row * 512u + uint(x)].rgb;
    part[tid] = s;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint w = 64u; w > 0u; w >>= 1) {
        if (tid < w) part[tid] += part[tid + w];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (tid == 0u) means[row] = float4(part[0] / 512.0f, 1.0f);
}
// supply_row + supply_state (+ prime mode) in one threadgroup (256 threads). The anode-voltage
// recurrence v_j = a v_(j-1) + (1 - a) target_j (a = decay) is affine with a constant factor, so
// v_j = a^(j+1) v_start + P_j with P the a-weighted prefix sum of (1 - a) target (parallel scan);
// the prime mode's three passes over the frame reuse P_last.
kernel void supply_fast(device const float4* means [[buffer(0)]], device const float4* now [[buffer(1)]],
                        device float4* next [[buffer(2)]], device float4* perRow [[buffer(3)]],
                        constant SupplyParams& P [[buffer(4)]], uint tid [[thread_index_in_threadgroup]]) {
    threadgroup float scan[2][256];
    threadgroup float red[256];
    threadgroup float4 st;
    threadgroup float vStart;
    int t = int(tid), R = P.rows;
    float drive = t < R ? dot(means[t].rgb, P.share.xyz) : 0.0f;
    red[t] = P.imax * drive;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int w = 128; w > 0; w >>= 1) {
        if (t < w) red[t] += red[t + w];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    float req = red[0];
    if (t == 0) {
        if (P.pad != 0.0f) {
            float requested = req * P.reqScale;
            st = float4(P.v0, requested, requested > P.ilim ? P.ilim / requested : 1.0f, 1.0f);
        } else st = now[0];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float4 s0 = st;
    float a = P.decay;
    // b_j = (1 - a) * target_j, scanned with weights a^o (Hillis-Steele).
    int cur = 0;
    scan[0][t] = t < R ? (1.0f - a) * (P.v0 - P.reff * P.imax * s0.z * drive) : 0.0f;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float ao = a;
    for (int o = 1; o < 256; o <<= 1) {
        float v = scan[cur][t];
        if (t >= o) v += ao * scan[cur][t - o];
        scan[1 - cur][t] = v;
        cur = 1 - cur;
        ao *= ao;
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (t == 0) {
        float aR = pow(a, float(R)), last = scan[cur][R - 1];
        float v = s0.x;
        if (P.pad != 0.0f) for (int i = 0; i < 3; i++) v = P.v0 + (aR * v + last - P.v0) * P.relax;
        vStart = v;
        float vEnd = P.v0 + (aR * v + last - P.v0) * P.relax;
        float requested = s0.y + (req * P.reqScale - s0.y) * P.ablFraction;
        if (P.pad != 0.0f) requested = s0.y;
        next[0] = float4(vEnd, requested, requested > P.ilim ? P.ilim / requested : 1.0f, 1.0f);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (t < R) {
        float v = pow(a, float(t + 1)) * vStart + scan[cur][t], m = sqrt(P.v0 / v);
        perRow[t] = float4(m, s0.z * pow(v / P.v0, P.n) / m, 0.0f, 1.0f);
    }
}
// supply_resample + spot_h + phosphor persistence of one row (512 threads). Spot: tap k of
// source sample j weighs e_|k| / sum with e_k = exp(-k^2 / 2s^2) = e1^(k^2), s from the source's
// own drive; each thread resamples and weighs its 7 sources itself. Persistence (fast path: in the
// drive domain): the frame's drive goes into its ring slot and the tube is driven by the
// persistence-weighted sum of the slots, i.e. the tube is applied to the weighted drive instead of
// weighting the tube's light (the same except for the beam-current spot growth of the < 12 %
// afterglow share). Also the x pass of the scatter source (scatter_drive) on the result.
struct PostParams { int supply; int spot; float k1; int persistence; int newSlot; int depth; int rows; int rx; };
static inline float3 postResample(device const float4* src, int py, int x, float2 k, int supply) {
    if (x < 0 || x >= 512) return float3(0.0f);
    if (supply == 0) return src[py * 512 + x].rgb;
    float sx = 256.0f + (float(x) + 0.5f - 256.0f) / k.x - 0.5f; int x0 = int(floor(sx)); float t = sx - float(x0);
    float3 a = (x0 >= 0 && x0 < 512) ? src[py * 512 + x0].rgb : float3(0.0f);
    float3 b = (x0 + 1 >= 0 && x0 + 1 < 512) ? src[py * 512 + x0 + 1].rgb : float3(0.0f);
    return glmix3(a, b, t) * k.y;
}
kernel void drive_post_fast(device const float4* src [[buffer(0)]], device const float4* perRow [[buffer(1)]],
                            device float4* outp [[buffer(2)]], constant PostParams& P [[buffer(3)]],
                            device half4* ring [[buffer(4)]], constant float4* w [[buffer(5)]],
                            constant float* wx [[buffer(6)]], device half4* blurX [[buffer(7)]],
                            uint tid [[thread_index_in_threadgroup]], uint row [[threadgroup_position_in_grid]]) {
    threadgroup float rowv[3 * 512];
    int px = int(tid), py = int(row), i = py * 512 + px;
    float2 k = P.supply != 0 ? perRow[py].xy : float2(1.0f);
    float3 v;
    if (P.spot != 0) {
        v = float3(0.0f);
        for (int d = -3; d <= 3; d++) {
            int j = px - d;
            if (j < 0 || j >= 512) continue;
            float3 u = postResample(src, py, j, k, P.supply);
            float3 sg = P.k1 * sqrt(max(u, 0.0f));
            float3 e1 = exp(-0.5f / max(sg * sg, 1e-30f));
            float3 e2 = e1 * e1; e2 *= e2; float3 e3 = e2 * e2 * e1;
            float3 ek = d == 0 ? float3(1.0f) : (abs(d) == 1 ? e1 : (abs(d) == 2 ? e2 : e3));
            float3 wk = ek / (1.0f + 2.0f * (e1 + e2 + e3));
            wk = select(wk, float3(d == 0 ? 1.0f : 0.0f), sg <= 1e-6f);
            v += u * wk;
        }
    } else v = postResample(src, py, px, k, P.supply);
    if (P.persistence != 0) {
        int area = 512 * P.rows;
        ring[P.newSlot * area + i] = half4(half3(v), 1.0h);
        float3 eff = v * w[P.newSlot].rgb;
        for (int s = 0; s < P.depth; s++) {
            float3 q = w[s].rgb;
            if (s == P.newSlot || all(q == 0.0f)) continue;
            eff += float3(ring[s * area + i].rgb) * q;
        }
        v = eff;
    }
    outp[i] = float4(v, 1.0f);
    rowv[px] = v.r; rowv[512 + px] = v.g; rowv[1024 + px] = v.b;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float3 s = float3(0.0f);
    int lo = max(-P.rx, -px), hi = min(P.rx, 511 - px);
    for (int d = lo; d <= hi; d++) { int j = px + d; s += float3(rowv[j], rowv[512 + j], rowv[1024 + j]) * wx[d + P.rx]; }
    blurX[i] = half4(half3(s), 1.0h);
}
// Scatter source (fast path): the 2 mm screen scatter of the emitted light, evaluated on the tube
// drive. The scatter Gaussian is ~8x wider than the beam spot, the detector and the slot pitch,
// so the scattered light is the drive blurred by sqrt(scatter^2 + spot^2 + detector^2 - bilinear)
// (in drive samples per axis; zero extension) times the mean emission per unit drive and the
// scatter fraction (kappa, from the plan). The x pass runs in drive_post_fast; this is the y pass.
struct ScatterParams { int rows; int ry; int pad0; int pad1; float4 kappa; };
kernel void scatter_drive(device const half4* src [[buffer(0)]], constant float* w [[buffer(1)]],
                          device half4* outp [[buffer(2)]], constant ScatterParams& P [[buffer(3)]],
                          uint2 g [[thread_position_in_grid]]) {
    int x = int(g.x), y = int(g.y);
    if (x >= 512 || y >= P.rows) return;
    float3 s = float3(0.0f);
    int lo = max(-P.ry, -y), hi = min(P.ry, P.rows - 1 - y);
    device const half4* c = src + y * 512 + x;
    for (int d = lo; d <= hi; d++) s += float3(c[d * 512].rgb) * w[d + P.ry];
    outp[y * 512 + x] = half4(half3(s * P.kappa.rgb), 1.0h);
}

// 64 bytes (the Vulkan push-constant budget). The drive is 512 samples wide.
struct FastTubeParams {
    int height; int ow; int oh; int xCount; int gCount; int hStride; float gain; float keep;
    float4 amb;    // ambient: (scale, diffuse fraction, normal slope x, normal slope y)
    float4 light;  // normalised light direction (w unused)
};
// hprog: h[(parity*height + y)*ow + x] = half4(R, G, B, 0), channel c from branch c*2 + parity.
// A threadgroup covers 64 columns x 16 rows, one thread per (parity, column, channel): the drive
// samples the tile reaches (tiles[] = first, last index) are staged in threadgroup memory once,
// each thread keeps its branch's taps in registers (N = the plan's tap count rounded up to 8, 12,
// 16 or 24; fast-plan padding taps point at the tile's first input with weight 0).
template <int N>
static inline void tubeHRows(threadgroup const float* rowv, int S, int ny, device const float2* taps, int n, int first,
                             device float* o, int ostride) {
    int idx[N]; float wt[N];
    for (int k = 0; k < N; k++) { float2 t = k < n ? taps[k] : float2(float(first), 0.0f); idx[k] = int(t.x) - first; wt[k] = t.y; }
    for (int r = 0; r < ny; r++) {
        threadgroup const float* rv = rowv + r * S;
        float sum = 0.0f;
        for (int k = 0; k < N; k++) sum += rv[idx[k]] * wt[k];
        o[r * ostride] = sum;
    }
}
kernel void tube_h_fast(device const float4* src [[buffer(0)]], device const float2* xmap [[buffer(1)]],
                        device float* h [[buffer(2)]], constant FastTubeParams& P [[buffer(3)]],
                        device const int2* tiles [[buffer(4)]], threadgroup float* rowv [[threadgroup(0)]],
                        uint2 tg [[threadgroup_position_in_grid]], uint lid [[thread_index_in_threadgroup]]) {
    int S = P.hStride;   // >= the widest tile span
    int PS = (16 * S + 31) / 32 * 32 + 11;   // channel plane stride: planes start in different banks
    int c = int(lid) % 3, pxl = (int(lid) / 3) % 64, parity = int(lid) / 192;
    int px = int(tg.x) * 64 + pxl, y0 = int(tg.y) * 16, ny = min(16, P.height - y0);
    int2 sp = tiles[tg.x];
    int span = sp.y - sp.x + 1;
    for (int i = int(lid); i < ny * span; i += 384) {
        int r = i / span, x = i - r * span;
        float4 v = src[(y0 + r) * 512 + sp.x + x];
        rowv[r * S + x] = v.x; rowv[PS + r * S + x] = v.y; rowv[2 * PS + r * S + x] = v.z;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (px >= P.ow) return;
    device const float2* taps = xmap + (px * 6 + c * 2 + parity) * P.xCount;
    device float* o = h + ((parity * P.height + y0) * P.ow + px) * 4 + c;
    threadgroup const float* rv = rowv + c * PS;
    if (P.xCount <= 8) tubeHRows<8>(rv, S, ny, taps, P.xCount, sp.x, o, P.ow * 4);
    else if (P.xCount <= 12) tubeHRows<12>(rv, S, ny, taps, P.xCount, sp.x, o, P.ow * 4);
    else if (P.xCount <= 16) tubeHRows<16>(rv, S, ny, taps, P.xCount, sp.x, o, P.ow * 4);
    else tubeHRows<24>(rv, S, ny, taps, P.xCount, sp.x, o, P.ow * 4);
}
// vprog (fixed or growth spot) + mixprog: emission, the scatter (bilinear from the drive-domain
// scatter source) and the ambient reflection, straight into the half-float output. Fast-plan
// tables (CRTTube.FastPlan): per output row and parity the contiguous input rows (first, count),
// per tap a cubic in the beam-current fraction u = h / colsum fitted to the growth-level tables
// (fixed spot: the constant weight, colinv = 0).
kernel void tube_v_fast(device const float4* h [[buffer(0)]], device const float4* vcoef [[buffer(1)]],
                        device const float4* colinv [[buffer(2)]], device const int2* vrows [[buffer(3)]],
                        device const half4* scatter [[buffer(4)]], device half4* outp [[buffer(5)]],
                        constant FastTubeParams& P [[buffer(6)]], uint2 g [[thread_position_in_grid]]) {
    int px = int(g.x), py = int(g.y);
    if (px >= P.ow || py >= P.oh) return;
    int plane = P.height * P.ow;
    float3 value = float3(0.0f);
    for (int parity = 0; parity < 2; parity++) {
        int2 fr = vrows[py * 2 + parity];
        device const float4* cf = vcoef + (py * 2 + parity) * P.gCount;
        float3 sc = colinv[parity * P.ow + px].rgb;
        device const float4* h0 = h + parity * plane + fr.x * P.ow + px;
        float3 sum = float3(0.0f);
        for (int k = 0; k < fr.y; k++) {
            float3 hv = float3(h0[k * P.ow].rgb);
            float4 c = cf[k];
            float3 u = clamp(hv * sc, 0.0f, 1.0f);
            sum += hv * (((c.w * u + c.z) * u + c.y) * u + c.x);
        }
        value += sum * P.gain;
    }
    // Scatter source sampled at the pixel centre in drive coordinates (512 x height samples).
    float sx = (float(px) + 0.5f) * 512.0f / float(P.ow) - 0.5f, sy = (float(py) + 0.5f) * float(P.height) / float(P.oh) - 0.5f;
    float fx0 = floor(sx), fy0 = floor(sy), fx = sx - fx0, fy = sy - fy0;
    int x0 = clamp(int(fx0), 0, 511), x1 = clamp(int(fx0) + 1, 0, 511);
    int y0 = clamp(int(fy0), 0, P.height - 1), y1 = clamp(int(fy0) + 1, 0, P.height - 1);
    float3 top = mix(float3(scatter[y0 * 512 + x0].rgb), float3(scatter[y0 * 512 + x1].rgb), fx);
    float3 bot = mix(float3(scatter[y1 * 512 + x0].rgb), float3(scatter[y1 * 512 + x1].rgb), fx);
    float nx = ((float(px) + 0.5f) / float(P.ow) * 2.0f - 1.0f) * P.amb.z;
    float ny = ((float(py) + 0.5f) / float(P.oh) * 2.0f - 1.0f) * P.amb.w;
    float cosine = max(0.0f, (nx * P.light.x + ny * P.light.y + P.light.z) / sqrt(nx * nx + ny * ny + 1.0f));
    float3 val = float3(P.amb.x * (P.amb.y + (1.0f - P.amb.y) * cosine)) + value * P.keep + mix(top, bot, fy);
    outp[py * P.ow + px] = half4(half3(val), 1.0h);
}
static inline float3 tubeAtH(device const half4* img, int x, int y, int ow, int oh) {
    x = clamp(x, 0, ow - 1); y = clamp(y, 0, oh - 1); return float3(img[y * ow + x].rgb);
}
static inline float3 showSampleH(device const half4* img, constant ShowParams& P, float2 frag) {
    float u = (frag.x - P.dst.x) / P.dst.z * P.src.z + P.src.x - 0.5f;
    float v = (frag.y - P.dst.y) / P.dst.w * P.src.w + P.src.y - 0.5f;
    float x0 = floor(u), y0 = floor(v), fx = u - x0, fy = v - y0;
    int ix = int(x0), iy = int(y0);
    float3 a = tubeAtH(img, ix, iy, P.ow, P.oh), b = tubeAtH(img, ix + 1, iy, P.ow, P.oh);
    float3 c = tubeAtH(img, ix, iy + 1, P.ow, P.oh), d = tubeAtH(img, ix + 1, iy + 1, P.ow, P.oh);
    return mix(mix(a, b, fx), mix(c, d, fx), fy);
}
fragment float4 show_fragment_h(ShowOut in [[stage_in]], device const half4* img [[buffer(0)]], constant ShowParams& P [[buffer(1)]]) {
    return float4(encodeSRGB(showSampleH(img, P, in.pos.xy)), 1.0f);
}
kernel void show_kernel_h(device const half4* img [[buffer(0)]], constant ShowParams& P [[buffer(1)]],
                          texture2d<float, access::write> target [[texture(0)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= target.get_width() || g.y >= target.get_height()) return;
    float2 frag = float2(g) + 0.5f;
    bool inside = frag.x >= P.dst.x && frag.y >= P.dst.y && frag.x < P.dst.x + P.dst.z && frag.y < P.dst.y + P.dst.w;
    target.write(inside ? float4(encodeSRGB(showSampleH(img, P, frag)), 1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f), g);
}
"""#
}
