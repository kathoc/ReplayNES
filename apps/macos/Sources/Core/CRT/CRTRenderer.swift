// GPU pipeline of the nesterm physical CRT ("CRT (physical model, experimental)"), ported from
// web/physical-worker.mjs + vendor/crt/backends/*-webgl.mjs to Metal compute:
//   PPU codes -> RF/IF (FFT overlap-save FIR, M3) -> receiver + AGC (M3b/M3c) -> [raster lines]
//   -> [supply/ABL (M4c)] -> [horizontal spot (M4C-SPOT-H)] -> tube detector + scatter (M1/M4a)
//   -> [phosphor persistence (M4b)] -> linear-light output buffer -> show (sRGB) into a target.
// Every per-frame stage runs on the GPU in the caller's command buffer (no worker round trip,
// no queue, no readback: the AGC loop that nesterm runs on the CPU after a readPixels runs as a
// one-thread kernel). Display-only: never touches emulation state.
// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md
import CoreGraphics
import Foundation
import Metal

final class CRTRenderer {
    /// physical-worker.mjs `effects` + `lines` (defaults as shipped in nesterm's web UI).
    struct Settings: Equatable {
        var lines = 240                 // 240 native, or 110...220 (reduced-line experiment)
        var beamGrowth = true           // scanlines get thicker where brighter (M4a vertical + M4C-SPOT-H)
        var persistence = true          // phosphor persistence (M4b)
        var supply = true               // bright screens widen and dim the picture (M4c)
        var antennaDbuv = 65.0          // signal strength 20...90 dBuV (M3-NOISE)
        var ambientLux = 40.0           // fixed in nesterm's UI

        static func validLines(_ l: Int) -> Bool { l == 240 || (110...220).contains(l) }
        var sanitized: Settings {
            var s = self
            if !Self.validLines(s.lines) { s.lines = 240 }
            s.antennaDbuv = min(90, max(20, s.antennaDbuv))
            s.ambientLux = min(500, max(0, s.ambientLux))
            return s
        }
    }

    enum Input {
        /// Raw 9-bit PPU codes (rn_video_indices) + Nestopia burst phase: the RF path ("pixels").
        case codes(UnsafePointer<UInt16>, burstPhase: UInt32)
        /// 256x240 BGRA8 RGB frame as nesterm's synthetic-RGB source ("ascii" path: no RF).
        case rgb(UnsafePointer<UInt32>)
        /// Linear RGB drive, 512 x 240 (verification: replaces the receiver output).
        case drive(UnsafePointer<SIMD4<Float>>)
    }

    enum CRTError: Error, LocalizedError {
        case metal(String)
        var errorDescription: String? { if case .metal(let m) = self { return "CRT (Metal): \(m)" }; return nil }
    }

    let device: MTLDevice
    private(set) var settings = Settings()
    private let pipes: [String: MTLComputePipelineState]
    private let show: MTLRenderPipelineState?

    // Receiver (fixed size).
    private let blocks = CRTRF.blocks
    private let ping, pong, kernelSpec, twiddles, volts, basis, carrier, stats, gains, agcState, prepared, rxOut: MTLBuffer
    // Input ring (shared storage, written by the CPU right before encoding).
    private static let inputSlots = 3
    private let codeBufs: [MTLBuffer], phaseBufs: [MTLBuffer], rgbBufs: [MTLBuffer]
    private var inputSlot = 0
    // Raster / supply / spot (per line count).
    private var rasterOut: MTLBuffer!
    private var supplyMeans: MTLBuffer!, supplyRow: MTLBuffer!, supplyOut: MTLBuffer!, spotOut: MTLBuffer!
    private var supplyState: [MTLBuffer] = []
    private var driveIn: MTLBuffer!   // .drive test input
    private var supplyCurrent = 0
    private var stageLines = 0
    // Tube (per output size / growth / ambient / lines).
    private struct Tube {
        let key: TubeKey
        let plan: CRTTube.Plan
        let xmap, vmap, colsum, kx, ky, ambient, horizontal, emission, scatterX, scatterY: MTLBuffer
        var ring: MTLBuffer?
    }
    private struct TubeKey: Equatable { var ow: Int; var oh: Int; var lines: Int; var growth: Bool; var ambient: Double }
    private var tube: Tube?
    private var pendingKey: TubeKey?
    private var inFlightKey: TubeKey?
    private var building = false
    private let buildQueue = DispatchQueue(label: "replaynes.crt.plan", qos: .userInitiated)
    private let lock = NSLock()
    private var builtTube: Tube?   // finished by buildQueue, adopted on the encoding thread
    // Persistence ring bookkeeping (tube-webgl.mjs slots).
    private struct Slot { var ring: Int; var ordinal: Int64 }
    private var slots: [Slot] = []
    private var appliedPersistence: Bool?
    private var lastOrdinal: Int64?
    /// Linear-light tube output of the last encoded frame (ow x oh float4), or nil before the first.
    private(set) var hasOutput = false
    /// Register-window scatter (same taps, order and weights); false = the direct port.
    var useFastScatter = true
    /// Threadgroup-memory FFT (same butterflies); false = the multi-pass port of rf-webgl.mjs.
    var useFastFFT = true
    /// Verification hooks: RF noise off (the CPU reference receiver has none) / tube-only growth.
    var noiseEnabled = true
    var disableSpotH = false

    var outputSize: (width: Int, height: Int)? { tube.map { ($0.plan.outputWidth, $0.plan.outputHeight) } }

    // MARK: setup

    private static var libraries: [UInt64: MTLLibrary] = [:]
    private static let libLock = NSLock()

    static func library(for device: MTLDevice) throws -> MTLLibrary {
        libLock.lock(); defer { libLock.unlock() }
        if let l = libraries[device.registryID] { return l }
        let opts = MTLCompileOptions()
        // IEEE float32 like WebGL highp: no reassociation / approximate transcendental functions.
        if #available(macOS 15.0, *) { opts.mathMode = .safe } else { opts.fastMathEnabled = false }
        let l = try device.makeLibrary(source: CRTShaders.source, options: opts)
        libraries[device.registryID] = l
        return l
    }

    init(device: MTLDevice, targetPixelFormat: MTLPixelFormat? = .bgra8Unorm) throws {
        self.device = device
        let lib = try Self.library(for: device)
        var p: [String: MTLComputePipelineState] = [:]
        for name in ["rf_forward_tg", "rf_inverse_tg", "rf_init_codes", "rf_butterfly2", "rf_butterfly", "rf_multiply", "rf_extract", "rx_reduce", "rx_agc",
                     "rx_prepare", "rx_decode", "raster_area", "supply_mean", "supply_row", "supply_state", "supply_resample",
                     "spot_h", "tube_h", "tube_v", "tube_v_growth", "tube_scatter", "tube_scatter_x16", "tube_scatter_y16", "tube_mix", "tube_lit", "tube_persist",
                     "show_kernel"] {
            guard let f = lib.makeFunction(name: name) else { throw CRTError.metal("missing kernel \(name)") }
            p[name] = try device.makeComputePipelineState(function: f)
        }
        pipes = p
        if let fmt = targetPixelFormat {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = lib.makeFunction(name: "show_vertex")
            d.fragmentFunction = lib.makeFunction(name: "show_fragment")
            d.colorAttachments[0].pixelFormat = fmt
            show = try device.makeRenderPipelineState(descriptor: d)
        } else { show = nil }

        func buf(_ bytes: Int, shared: Bool = false) throws -> MTLBuffer {
            guard let b = device.makeBuffer(length: max(16, bytes), options: shared ? .storageModeShared : .storageModePrivate)
            else { throw CRTError.metal("buffer \(bytes)") }
            return b
        }
        func constBuf<T>(_ a: [T]) throws -> MTLBuffer {
            guard let b = a.withUnsafeBytes({ device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared) })
            else { throw CRTError.metal("const buffer") }
            return b
        }
        let n = CRTRF.fftSize
        ping = try buf(n * blocks * 8); pong = try buf(n * blocks * 8)
        kernelSpec = try constBuf(CRTRF.kernelSpectrum)
        twiddles = try constBuf(CRTRF.twiddles)
        volts = try constBuf(CRTComposite.voltageLUT)
        // receiver-webgl.mjs basis: (cos, sin)(2*pi*n/12) for n < 2728, float32.
        basis = try constBuf((0..<2728).map { SIMD2(Float(cos(2 * Double.pi * Double($0) / 12)), Float(sin(2 * Double.pi * Double($0) / 12))) })
        carrier = try buf(2728 * 240 * 8)
        stats = try buf(240 * 16)
        gains = try buf(240 * 4)
        agcState = try buf(16, shared: true)
        prepared = try buf(682 * 240 * 16)
        rxOut = try buf(512 * 240 * 16)
        codeBufs = try (0..<Self.inputSlots).map { _ in try buf(256 * 240 * 2, shared: true) }
        phaseBufs = try (0..<Self.inputSlots).map { _ in try buf(240 * 4, shared: true) }
        rgbBufs = try (0..<Self.inputSlots).map { _ in try buf(256 * 240 * 4, shared: true) }
        resetReceiver()
    }

    // MARK: configuration

    /// Applies settings and the wanted tube output size. `synchronous` builds the tube plan on the
    /// calling thread (export, tests); otherwise it is built in the background and adopted by a
    /// later encode (the previous plan keeps rendering until then, like nesterm's worker).
    func configure(settings newSettings: Settings, outputWidth: Int, outputHeight: Int, synchronous: Bool = false) {
        let s = newSettings.sanitized
        if s.lines != settings.lines { stageLines = 0 }  // raster/supply/spot rebuilt (nesterm: rasterChanged)
        settings = s
        let key = TubeKey(ow: max(64, outputWidth), oh: max(48, outputHeight), lines: s.lines, growth: s.beamGrowth, ambient: s.ambientLux)
        if synchronous {
            if tube?.key != key { tube = try? makeTube(key); appliedPersistence = nil }
            return
        }
        lock.lock()
        // Latest wanted plan: queued > being built > built but not adopted > in use.
        let target = pendingKey ?? inFlightKey ?? builtTube?.key ?? tube?.key
        if target == key { lock.unlock(); return }
        pendingKey = key
        let start = !building
        if start { building = true }
        lock.unlock()
        if start { buildQueue.async { [weak self] in self?.buildLoop() } }
    }

    private func buildLoop() {
        while true {
            lock.lock()
            guard let key = pendingKey else { building = false; lock.unlock(); return }
            pendingKey = nil
            inFlightKey = key
            lock.unlock()
            let t = try? makeTube(key)
            lock.lock()
            if let t, pendingKey == nil { builtTube = t }
            inFlightKey = nil
            lock.unlock()
        }
    }

    private func makeTube(_ k: TubeKey) throws -> Tube {
        let plan = CRTTube.makePlan(lines: k.lines, outputWidth: k.ow, outputHeight: k.oh, ambientLux: k.ambient, beamGrowth: k.growth ? 1 : 0)
        func constBuf<T>(_ a: [T]) throws -> MTLBuffer {
            if a.isEmpty { return device.makeBuffer(length: 16, options: .storageModeShared)! }
            guard let b = a.withUnsafeBytes({ device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared) })
            else { throw CRTError.metal("plan buffer") }
            return b
        }
        func img(_ w: Int, _ h: Int) throws -> MTLBuffer {
            guard let b = device.makeBuffer(length: w * h * 16, options: .storageModePrivate) else { throw CRTError.metal("image \(w)x\(h)") }
            return b
        }
        let ow = plan.outputWidth, oh = plan.outputHeight
        return Tube(key: k, plan: plan,
                    xmap: try constBuf(plan.xmap), vmap: try constBuf(plan.growth ? plan.gmap : plan.ymap),
                    colsum: try constBuf(plan.colsum), kx: try constBuf(plan.kernels[0].weights), ky: try constBuf(plan.kernels[1].weights),
                    ambient: try constBuf(plan.ambient), horizontal: try img(ow, plan.height * 2),
                    emission: try img(ow, oh), scatterX: try img(ow, oh), scatterY: try img(ow, oh), ring: nil)
    }

    private func ensureStages() throws {
        let rows = settings.lines
        if stageLines == rows { return }
        func img(_ h: Int) throws -> MTLBuffer {
            guard let b = device.makeBuffer(length: 512 * h * 16, options: .storageModePrivate) else { throw CRTError.metal("stage") }
            return b
        }
        rasterOut = try img(rows); supplyOut = try img(rows); spotOut = try img(rows)
        supplyMeans = device.makeBuffer(length: rows * 16, options: .storageModePrivate)
        supplyRow = device.makeBuffer(length: rows * 16, options: .storageModePrivate)
        driveIn = device.makeBuffer(length: 512 * 240 * 16, options: .storageModeShared)
        supplyState = (0..<2).map { _ in device.makeBuffer(length: 16, options: .storageModeShared)! }
        stageLines = rows
        resetSupply()
    }

    // MARK: state resets (physical-worker 'reset': receiver.reset(), tube.resetPersistence(), supply.reset())

    func reset() {
        resetReceiver()
        slots = []
        resetSupply()
        lastOrdinal = nil
    }
    private func resetReceiver() {
        let p = agcState.contents().assumingMemoryBound(to: Float.self)
        p[0] = Float(CRTAGC.initialGain); p[1] = 0; p[2] = 0; p[3] = 0
    }
    private func resetSupply() {
        for b in supplyState {
            let p = b.contents().assumingMemoryBound(to: SIMD4<Float>.self)
            p[0] = SIMD4(Float(CRTSupply.v0), 0, 1, 1)
        }
        supplyCurrent = 0
    }

    // MARK: per-frame encoding

    private struct RFParams { var hop, overlap, delay, blocks: Int32; var scale, signalLevel, noiseSigma: Float; var noiseKey: Int32 }
    private struct FFTParams { var span: Int32; var signValue: Float; var blocks: Int32; var pad: Int32 = 0 }
    private struct AGCParams { var lineSeconds, attack, release, minGain, maxGain: Float; var enabled, delay, pad: Int32 }
    private struct RasterParams { var lines, decode, rgbSource, pad: Int32 }
    private struct SupplyParams {
        var width, rows: Int32; var decay, v0, reff, imax, n, relax, reqScale, ablFraction, ilim, pad: Float; var share: SIMD4<Float>
    }
    private struct SpotParams { var width, rows, radius: Int32; var k1: Float }
    private struct TubeParams {
        var width, height, ow, oh, xCount, yCount, gCount, levels: Int32
        var gain, scatterFraction, keep: Float; var radius, depth, pad0, pad1, pad2: Int32
    }
    struct ShowParams { var dst: SIMD4<Float>; var src: SIMD4<Float>; var ow, oh, pad0, pad1: Int32 }

    /// Encodes one emulated frame. `ordinal` is the machine frame ordinal (persistence history
    /// and per-frame RF noise key, like nesterm's packet.timing.frameOrdinal); a non-increasing
    /// ordinal is a discontinuity and resets the receiver/supply/persistence state.
    /// Returns false when no tube plan is ready yet (nothing encoded).
    @discardableResult
    func encode(_ input: Input, ordinal: UInt64, into cb: MTLCommandBuffer) -> Bool {
        lock.lock()
        if let b = builtTube { builtTube = nil; tube = b; appliedPersistence = nil; hasOutput = false }
        lock.unlock()
        guard var t = tube, (try? ensureStages()) != nil, let enc = cb.makeComputeCommandEncoder() else { return false }
        let ord = Int64(clamping: ordinal)
        if let last = lastOrdinal, ord <= last {
            resetReceiver(); resetSupply(); slots = []
        }
        lastOrdinal = ord
        if settings.persistence != appliedPersistence {
            // tube.setPersistence(): allocate the ring on demand; always restarts history.
            if settings.persistence && t.ring == nil {
                t.ring = device.makeBuffer(length: CRTPhosphor.depth * t.plan.outputWidth * t.plan.outputHeight * 8, options: .storageModePrivate)
                tube = t
            }
            slots = []
            appliedPersistence = settings.persistence
        }
        let slot = inputSlot
        inputSlot = (inputSlot + 1) % Self.inputSlots
        let rows = settings.lines

        func dispatch(_ name: String, _ w: Int, _ h: Int = 1, _ bind: (MTLComputeCommandEncoder) -> Void) {
            let ps = pipes[name]!
            enc.setComputePipelineState(ps)
            bind(enc)
            let tw = min(ps.threadExecutionWidth, max(1, w))
            let th = max(1, min(ps.maxTotalThreadsPerThreadgroup / tw, h))
            enc.dispatchThreads(MTLSize(width: w, height: h, depth: 1), threadsPerThreadgroup: MTLSize(width: tw, height: th, depth: 1))
        }

        var drive: MTLBuffer
        switch input {
        case .codes(let codes, let burst):
            codeBufs[slot].contents().copyMemory(from: codes, byteCount: 256 * 240 * 2)
            let ph = CRTComposite.rowPhases(burstPhase: burst)
            ph.withUnsafeBytes { phaseBufs[slot].contents().copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
            // M3-NOISE: receiver thermal noise from the antenna level; seed 1, keyed by frame ordinal.
            let sigma = CRTRF.noiseSigma(cnrDb: CRTRF.carrierToNoiseDb(settings.antennaDbuv), signalLevel: 1)
            var rp = RFParams(hop: Int32(CRTRF.hop), overlap: Int32(CRTRF.overlap), delay: Int32(CRTRF.filter.delaySamples),
                              blocks: Int32(blocks), scale: Float(CRTRF.modulationDepth / (CRTComposite.white - CRTComposite.sync)),
                              signalLevel: 1, noiseSigma: noiseEnabled ? Float(sigma) : 0, noiseKey: CRTRF.noiseKey(seed: 1, frame: ordinal))
            if useFastFFT && device.maxThreadgroupMemoryLength >= 4096 * 8 {
                for (name, isForward) in [("rf_forward_tg", true), ("rf_inverse_tg", false)] {
                    let ps = pipes[name]!
                    enc.setComputePipelineState(ps)
                    if isForward {
                        enc.setBuffer(ping, offset: 0, index: 0); enc.setBuffer(codeBufs[slot], offset: 0, index: 1)
                        enc.setBuffer(phaseBufs[slot], offset: 0, index: 2); enc.setBuffer(volts, offset: 0, index: 3)
                        enc.setBytes(&rp, length: MemoryLayout<RFParams>.stride, index: 4); enc.setBuffer(twiddles, offset: 0, index: 5)
                    } else {
                        enc.setBuffer(ping, offset: 0, index: 0); enc.setBuffer(carrier, offset: 0, index: 1)
                        enc.setBuffer(kernelSpec, offset: 0, index: 2); enc.setBytes(&rp, length: MemoryLayout<RFParams>.stride, index: 3)
                        enc.setBuffer(twiddles, offset: 0, index: 4)
                    }
                    enc.setThreadgroupMemoryLength(4096 * 8, index: 0)
                    let tg = min(1024, ps.maxTotalThreadsPerThreadgroup)
                    enc.dispatchThreadgroups(MTLSize(width: blocks, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: tg, height: 1, depth: 1))
                }
            } else {
                dispatch("rf_init_codes", 4096, blocks) {
                    $0.setBuffer(ping, offset: 0, index: 0); $0.setBuffer(codeBufs[slot], offset: 0, index: 1)
                    $0.setBuffer(phaseBufs[slot], offset: 0, index: 2); $0.setBuffer(volts, offset: 0, index: 3)
                    $0.setBytes(&rp, length: MemoryLayout<RFParams>.stride, index: 4)
                }
                var a = ping, b = pong
                func fft(_ sign: Float) {
                    var span = 2
                    while span <= 4096 {
                        var fp = FFTParams(span: Int32(span), signValue: sign, blocks: Int32(blocks))
                        let name = span * 2 <= 4096 ? "rf_butterfly2" : "rf_butterfly"
                        dispatch(name, 4096, blocks) {
                            $0.setBuffer(a, offset: 0, index: 0); $0.setBuffer(b, offset: 0, index: 1)
                            $0.setBuffer(twiddles, offset: 0, index: 2); $0.setBytes(&fp, length: MemoryLayout<FFTParams>.stride, index: 3)
                        }
                        swap(&a, &b)
                        span *= 4
                    }
                }
                fft(-1)
                var mp = FFTParams(span: 0, signValue: 0, blocks: Int32(blocks))
                dispatch("rf_multiply", 4096, blocks) {
                    $0.setBuffer(a, offset: 0, index: 0); $0.setBuffer(b, offset: 0, index: 1)
                    $0.setBuffer(kernelSpec, offset: 0, index: 2); $0.setBytes(&mp, length: MemoryLayout<FFTParams>.stride, index: 3)
                }
                swap(&a, &b)
                fft(1)
                let inv = a
                dispatch("rf_extract", 2728, 240) {
                    $0.setBuffer(inv, offset: 0, index: 0); $0.setBuffer(carrier, offset: 0, index: 1)
                    $0.setBytes(&rp, length: MemoryLayout<RFParams>.stride, index: 2)
                }
            }
            dispatch("rx_reduce", 240) {
                $0.setBuffer(carrier, offset: 0, index: 0); $0.setBuffer(basis, offset: 0, index: 1); $0.setBuffer(stats, offset: 0, index: 2)
            }
            var ap = AGCParams(lineSeconds: Float(Double(CRTComposite.lineSamples) / CRTRF.sampleRateHz),
                               attack: Float(CRTAGC.attackSeconds), release: Float(CRTAGC.releaseSeconds),
                               minGain: Float(CRTAGC.minGain), maxGain: Float(CRTAGC.maxGain), enabled: 1,
                               delay: Int32(CRTRF.filter.delaySamples), pad: 0)
            dispatch("rx_agc", 1) {
                $0.setBuffer(stats, offset: 0, index: 0); $0.setBuffer(gains, offset: 0, index: 1)
                $0.setBuffer(agcState, offset: 0, index: 2); $0.setBytes(&ap, length: MemoryLayout<AGCParams>.stride, index: 3)
            }
            dispatch("rx_prepare", 682, 240) {
                $0.setBuffer(carrier, offset: 0, index: 0); $0.setBuffer(basis, offset: 0, index: 1)
                $0.setBuffer(stats, offset: 0, index: 2); $0.setBuffer(prepared, offset: 0, index: 3)
            }
            dispatch("rx_decode", 512, 240) {
                $0.setBuffer(prepared, offset: 0, index: 0); $0.setBuffer(stats, offset: 0, index: 1)
                $0.setBuffer(gains, offset: 0, index: 2); $0.setBuffer(rxOut, offset: 0, index: 3)
            }
            drive = rxOut
        case .rgb(let px):
            rgbBufs[slot].contents().copyMemory(from: px, byteCount: 256 * 240 * 4)
            var rp2 = RasterParams(lines: Int32(rows), decode: 1, rgbSource: 1, pad: 0)
            dispatch("raster_area", 512, rows) {
                $0.setBuffer(rxOut, offset: 0, index: 0); $0.setBuffer(rgbBufs[slot], offset: 0, index: 1)
                $0.setBuffer(rasterOut, offset: 0, index: 2); $0.setBytes(&rp2, length: MemoryLayout<RasterParams>.stride, index: 3)
            }
            drive = rasterOut
        case .drive(let px):
            driveIn.contents().copyMemory(from: px, byteCount: 512 * 240 * 16)
            drive = driveIn
        }
        if case .rgb = input {} else if rows != 240 {
            // RasterWebGL.process(texture): the native 240-line RF path passes through untouched.
            var rp2 = RasterParams(lines: Int32(rows), decode: 0, rgbSource: 0, pad: 0)
            let src = drive
            dispatch("raster_area", 512, rows) {
                $0.setBuffer(src, offset: 0, index: 0); $0.setBuffer(src, offset: 0, index: 1)
                $0.setBuffer(rasterOut, offset: 0, index: 2); $0.setBytes(&rp2, length: MemoryLayout<RasterParams>.stride, index: 3)
            }
            drive = rasterOut
        }

        // M4c: anode-voltage load and ABL act on the drive before the tube.
        if settings.supply {
            let f = CRTSupply.self
            let line = f.lineS * f.visibleLines / Double(rows)
            var sp = SupplyParams(width: 512, rows: Int32(rows), decay: Float(exp(-line / f.tauS)), v0: Float(f.v0), reff: Float(f.reff),
                                  imax: Float(f.imax), n: Float(f.n), relax: Float(exp(-(f.totalLines - f.visibleLines) * f.lineS / f.tauS)),
                                  reqScale: Float(f.visibleLines / f.totalLines / Double(rows)), ablFraction: Float(-expm1(-f.frameS / f.tauAblS)),
                                  ilim: Float(f.ilim), pad: 0, share: SIMD4(Float(f.share[0]), Float(f.share[1]), Float(f.share[2]), 0))
            let now = supplyState[supplyCurrent], next = supplyState[1 - supplyCurrent]
            let src = drive
            dispatch("supply_mean", rows) {
                $0.setBuffer(src, offset: 0, index: 0); $0.setBuffer(supplyMeans, offset: 0, index: 1)
                $0.setBytes(&sp, length: MemoryLayout<SupplyParams>.stride, index: 2)
            }
            dispatch("supply_row", rows) {
                $0.setBuffer(supplyMeans, offset: 0, index: 0); $0.setBuffer(now, offset: 0, index: 1)
                $0.setBuffer(supplyRow, offset: 0, index: 2); $0.setBytes(&sp, length: MemoryLayout<SupplyParams>.stride, index: 3)
            }
            dispatch("supply_state", 1) {
                $0.setBuffer(supplyMeans, offset: 0, index: 0); $0.setBuffer(now, offset: 0, index: 1)
                $0.setBuffer(next, offset: 0, index: 2); $0.setBytes(&sp, length: MemoryLayout<SupplyParams>.stride, index: 3)
            }
            dispatch("supply_resample", 512, rows) {
                $0.setBuffer(src, offset: 0, index: 0); $0.setBuffer(supplyRow, offset: 0, index: 1)
                $0.setBuffer(supplyOut, offset: 0, index: 2); $0.setBytes(&sp, length: MemoryLayout<SupplyParams>.stride, index: 3)
            }
            supplyCurrent = 1 - supplyCurrent
            drive = supplyOut
        }
        // M4C-SPOT-H: horizontal counterpart of the beam-current spot growth (same toggle).
        if settings.beamGrowth && !disableSpotH {
            var spp = SpotParams(width: 512, rows: Int32(rows), radius: Int32(CRTTube.spotHRadius),
                                 k1: Float(CRTTube.extraSigmaSamples(1, growth: 1, width: 512)))
            let src = drive
            dispatch("spot_h", 512, rows) {
                $0.setBuffer(src, offset: 0, index: 0); $0.setBuffer(spotOut, offset: 0, index: 1)
                $0.setBytes(&spp, length: MemoryLayout<SpotParams>.stride, index: 2)
            }
            drive = spotOut
        }

        lastDriveAfterStages = drive
        // Tube.
        let p = t.plan, ow = p.outputWidth, oh = p.outputHeight
        var tp = TubeParams(width: Int32(p.width), height: Int32(p.height), ow: Int32(ow), oh: Int32(oh), xCount: Int32(p.xCount),
                            yCount: Int32(p.yCount), gCount: Int32(p.gCount), levels: Int32(p.growthLevels), gain: Float(p.gain),
                            scatterFraction: Float(p.scatterFraction), keep: Float(1 - p.scatterFraction), radius: 0,
                            depth: Int32(CRTPhosphor.depth), pad0: 0, pad1: 0, pad2: 0)
        let src = drive
        dispatch("tube_h", ow, p.height * 2) {
            $0.setBuffer(src, offset: 0, index: 0); $0.setBuffer(t.xmap, offset: 0, index: 1)
            $0.setBuffer(t.horizontal, offset: 0, index: 2); $0.setBytes(&tp, length: MemoryLayout<TubeParams>.stride, index: 3)
        }
        if p.growth {
            dispatch("tube_v_growth", ow, oh) {
                $0.setBuffer(t.horizontal, offset: 0, index: 0); $0.setBuffer(t.vmap, offset: 0, index: 1)
                $0.setBuffer(t.colsum, offset: 0, index: 2); $0.setBuffer(t.emission, offset: 0, index: 3)
                $0.setBytes(&tp, length: MemoryLayout<TubeParams>.stride, index: 4)
            }
        } else {
            dispatch("tube_v", ow, oh) {
                $0.setBuffer(t.horizontal, offset: 0, index: 0); $0.setBuffer(t.vmap, offset: 0, index: 1)
                $0.setBuffer(t.emission, offset: 0, index: 2); $0.setBytes(&tp, length: MemoryLayout<TubeParams>.stride, index: 3)
            }
        }
        for axis in 0..<2 {
            var ax = Int32(axis)
            let radius = p.kernels[axis].radius
            tp.radius = Int32(radius)
            let from = axis == 0 ? t.emission : t.scatterX, to = axis == 0 ? t.scatterX : t.scatterY, w = axis == 0 ? t.kx : t.ky
            if useFastScatter {
                let name = axis == 0 ? "tube_scatter_x16" : "tube_scatter_y16"
                dispatch(name, axis == 0 ? (ow + 15) / 16 : ow, axis == 0 ? oh : (oh + 15) / 16) {
                    $0.setBuffer(from, offset: 0, index: 0); $0.setBuffer(w, offset: 0, index: 1); $0.setBuffer(to, offset: 0, index: 2)
                    $0.setBytes(&tp, length: MemoryLayout<TubeParams>.stride, index: 3)
                }
            } else {
                dispatch("tube_scatter", ow, oh) {
                    $0.setBuffer(from, offset: 0, index: 0); $0.setBuffer(w, offset: 0, index: 1); $0.setBuffer(to, offset: 0, index: 2)
                    $0.setBytes(&tp, length: MemoryLayout<TubeParams>.stride, index: 3); $0.setBytes(&ax, length: 4, index: 4)
                }
            }
        }
        // The x-scatter buffer is free again: it doubles as the output image.
        let out = t.scatterX
        if settings.persistence, let ring = t.ring {
            let depth = CRTPhosphor.depth
            let next = ord
            if let first = slots.first, next <= first.ordinal { slots = [] }
            var kept: [Slot] = []
            for s in slots {
                let newer = kept.last?.ordinal ?? next
                if next - newer + 1 <= Int64(depth - 1) { kept.append(s) } else { break }
            }
            slots = kept
            let used = Set(kept.map { $0.ring })
            var r = 0
            while used.contains(r) { r += 1 }
            var ringIndex = Int32(r)
            dispatch("tube_lit", ow, oh) {
                $0.setBuffer(t.emission, offset: 0, index: 0); $0.setBuffer(t.scatterY, offset: 0, index: 1)
                $0.setBuffer(ring, offset: 0, index: 2); $0.setBytes(&tp, length: MemoryLayout<TubeParams>.stride, index: 3)
                $0.setBytes(&ringIndex, length: 4, index: 4)
            }
            slots.insert(Slot(ring: r, ordinal: next), at: 0)
            var w = persistenceWeights()
            dispatch("tube_persist", ow, oh) {
                $0.setBuffer(ring, offset: 0, index: 0); $0.setBuffer(t.ambient, offset: 0, index: 1)
                $0.setBuffer(out, offset: 0, index: 2); $0.setBytes(&tp, length: MemoryLayout<TubeParams>.stride, index: 3)
                $0.setBytes(&w, length: MemoryLayout<SIMD4<Float>>.stride * w.count, index: 4)
            }
        } else {
            dispatch("tube_mix", ow, oh) {
                $0.setBuffer(t.emission, offset: 0, index: 0); $0.setBuffer(t.scatterY, offset: 0, index: 1)
                $0.setBuffer(t.ambient, offset: 0, index: 2); $0.setBuffer(out, offset: 0, index: 3)
                $0.setBytes(&tp, length: MemoryLayout<TubeParams>.stride, index: 4)
            }
        }
        enc.endEncoding()
        hasOutput = true
        return true
    }

    /// tube-webgl.mjs persistenceWeights(): slot i (newest first) also stands for missing ordinals
    /// up to the next newer slot (M4B-GAP). Float32 accumulation like the JS Float32Array.
    private func persistenceWeights() -> [SIMD4<Float>] {
        let depth = CRTPhosphor.depth
        var w = [SIMD4<Float>](repeating: .zero, count: depth)
        guard let k = slots.first?.ordinal else { return w }
        for (i, slot) in slots.enumerated() {
            let newest = i == 0 ? 0 : k - slots[i - 1].ordinal + 1, oldest = k - slot.ordinal
            let hi = min(oldest, Int64(depth - 1))
            if newest > hi { continue }
            for m in newest...hi { for c in 0..<3 { w[slot.ring][c] = Float(Double(w[slot.ring][c]) + CRTPhosphor.fractions[c][Int(m)]) } }
        }
        return w
    }

    // MARK: verification readback (tests only; never used by the live path)

    enum Stage { case receiver, tubeInput, emission, output }
    func read(_ stage: Stage) -> [SIMD4<Float>] {
        let src: MTLBuffer?
        switch stage {
        case .receiver: src = rxOut
        case .tubeInput: src = lastDriveAfterStages
        case .emission: src = tube?.emission
        case .output: src = outputBuffer
        }
        guard let src, let q = device.makeCommandQueue(), let cb = q.makeCommandBuffer(), let blit = cb.makeBlitCommandEncoder(),
              let dst = device.makeBuffer(length: src.length, options: .storageModeShared) else { return [] }
        blit.copy(from: src, sourceOffset: 0, to: dst, destinationOffset: 0, size: src.length)
        blit.endEncoding(); cb.commit(); cb.waitUntilCompleted()
        let n = src.length / 16
        return Array(UnsafeBufferPointer(start: dst.contents().assumingMemoryBound(to: SIMD4<Float>.self), count: n))
    }
    private var lastDriveAfterStages: MTLBuffer?

    // MARK: presentation

    /// Linear-light output image (ow x oh float4) of the last encoded frame.
    var outputBuffer: MTLBuffer? { hasOutput ? tube?.scatterX : nil }

    /// showprog into a render pass: `dst` = destination rectangle in target pixels (origin top-left),
    /// `cropRows` = tube rows hidden at the top and bottom (overscan).
    func encodeShow(_ enc: MTLRenderCommandEncoder, targetSize: CGSize, dst: CGRect, cropFraction: Double) {
        guard let show, let t = tube, hasOutput else { return }
        var sp = showParams(t, dst: dst, cropFraction: cropFraction)
        var ndc = SIMD4<Float>(Float(dst.minX / targetSize.width * 2 - 1), Float(1 - dst.maxY / targetSize.height * 2),
                               Float(dst.width / targetSize.width * 2), Float(dst.height / targetSize.height * 2))
        enc.setRenderPipelineState(show)
        enc.setVertexBytes(&ndc, length: 16, index: 0)
        enc.setFragmentBuffer(t.scatterX, offset: 0, index: 0)
        enc.setFragmentBytes(&sp, length: MemoryLayout<ShowParams>.stride, index: 1)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }

    /// showprog into a writable texture via compute (export / Syphon / tests); outside `dst` is black.
    func encodeShow(into texture: MTLTexture, cb: MTLCommandBuffer, dst: CGRect, cropFraction: Double) {
        guard let t = tube, hasOutput, let enc = cb.makeComputeCommandEncoder() else { return }
        var sp = showParams(t, dst: dst, cropFraction: cropFraction)
        let ps = pipes["show_kernel"]!
        enc.setComputePipelineState(ps)
        enc.setBuffer(t.scatterX, offset: 0, index: 0)
        enc.setBytes(&sp, length: MemoryLayout<ShowParams>.stride, index: 1)
        enc.setTexture(texture, index: 0)
        enc.dispatchThreads(MTLSize(width: texture.width, height: texture.height, depth: 1),
                            threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
        enc.endEncoding()
    }

    private func showParams(_ t: Tube, dst: CGRect, cropFraction: Double) -> ShowParams {
        let ow = Double(t.plan.outputWidth), oh = Double(t.plan.outputHeight)
        let cy = oh * cropFraction
        return ShowParams(dst: SIMD4(Float(dst.minX), Float(dst.minY), Float(dst.width), Float(dst.height)),
                          src: SIMD4(0, Float(cy), Float(ow), Float(oh - 2 * cy)),
                          ow: Int32(ow), oh: Int32(oh), pad0: 0, pad1: 0)
    }

    /// Tube output size that maps 1:1 onto a destination rectangle of `dst` pixels showing the
    /// rows between the overscan crops, capped at `maxWidth` (4:3 full raster).
    static func tubeSize(forDestination dst: CGSize, cropFraction: Double, maxWidth: Int) -> (Int, Int) {
        let fullH = dst.height / max(0.5, 1 - 2 * cropFraction)
        var w = min(dst.width, fullH * 4 / 3)
        w = max(256, min(Double(maxWidth), (w / 4).rounded() * 4))
        return (Int(w), Int(w) * 3 / 4)
    }
}
