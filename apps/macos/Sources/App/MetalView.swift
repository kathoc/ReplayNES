// Metal game viewport: 256x240 BGRA texture, nearest-neighbour, optional integer scale,
// 8:7 pixel aspect and overscan hiding, or the physical CRT model (CRTRenderer, nesterm port).
// Presentation only: frame drops/dupes here never affect emulation.
// The emulation thread drives it from its CAMetalDisplayLink (EmulationController): each
// refresh that starts a frame renders and presents the just-emulated picture for exactly that
// refresh; the others present the same picture again. Never the main thread, so SwiftUI / AppKit
// work cannot delay a frame (docs/FRAME_PACING.md).
// SPDX-License-Identifier: GPL-2.0-or-later
import Metal
import QuartzCore
import SwiftUI

struct DisplayOptions: Equatable {
    var integerScale = true
    var pixelAspect87 = false
    var hideOverscan = true
}

/// GPU execution of a new frame's command buffers (host-clock seconds, earliest start to latest
/// end; 0 until one completed). Written by Metal completion handlers, read by presented handlers.
final class GPUSpan {
    private let lock = NSLock()
    private var start_ = 0.0, end_ = 0.0
    func extend(_ s: Double, _ e: Double) {
        guard e > s else { return }
        lock.lock()
        start_ = start_ == 0 ? s : min(start_, s)
        end_ = max(end_, e)
        lock.unlock()
    }
    var times: (start: Double, end: Double) { lock.lock(); defer { lock.unlock() }; return (start_, end_) }
}

/// Emulation thread only, except `options` (any thread).
final class GameRenderer {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let pillPipeline: MTLRenderPipelineState
    private let texture: MTLTexture
    let frames: FrameBuffer
    private let latency: LatencyMeter
    private var seenSeq: UInt64 = .max
    private let optionsLock = NSLock()
    private var options_ = DisplayOptions()
    var options: DisplayOptions {
        get { optionsLock.lock(); defer { optionsLock.unlock() }; return options_ }
        set { optionsLock.lock(); options_ = newValue; optionsLock.unlock() }
    }

    private static let shader = """
    #include <metal_stdlib>
    using namespace metal;
    struct VOut { float4 pos [[position]]; float2 uv; };
    vertex VOut vmain(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]], constant float4 &uvr [[buffer(1)]]) {
        float2 c = float2(float(vid & 1), float(vid >> 1));
        VOut o;
        o.pos = float4(rect.x + c.x * rect.z, rect.y + c.y * rect.w, 0.0, 1.0);
        o.uv = float2(uvr.x + c.x * uvr.z, uvr.y + (1.0 - c.y) * uvr.w);
        return o;
    }
    fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {
        constexpr sampler s(filter::nearest, address::clamp_to_edge);
        return float4(tex.sample(s, in.uv).rgb, 1.0);
    }
    // Menu pill: premultiplied BGRA picture, faded by `alpha`.
    fragment float4 fpill(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], constant float &alpha [[buffer(0)]]) {
        constexpr sampler s(filter::linear, address::clamp_to_edge);
        return tex.sample(s, in.uv) * alpha;
    }
    """

    init?(frames: FrameBuffer, latency: LatencyMeter) {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { return nil }
        self.device = device
        self.queue = queue
        self.frames = frames
        self.latency = latency
        do {
            // Compiled at runtime from source: no dependency on the offline Metal toolchain.
            let lib = try device.makeLibrary(source: Self.shader, options: nil)
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = lib.makeFunction(name: "vmain")
            d.fragmentFunction = lib.makeFunction(name: "fmain")
            d.colorAttachments[0].pixelFormat = .bgra8Unorm
            pipeline = try device.makeRenderPipelineState(descriptor: d)
            let pd = MTLRenderPipelineDescriptor()
            pd.vertexFunction = lib.makeFunction(name: "vmain")
            pd.fragmentFunction = lib.makeFunction(name: "fpill")
            let ca = pd.colorAttachments[0]!
            ca.pixelFormat = .bgra8Unorm
            ca.isBlendingEnabled = true
            ca.sourceRGBBlendFactor = .one
            ca.sourceAlphaBlendFactor = .one
            ca.destinationRGBBlendFactor = .oneMinusSourceAlpha
            ca.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            pillPipeline = try device.makeRenderPipelineState(descriptor: pd)
        } catch {
            NSLog("ReplayNES: Metal pipeline failed: \(error)")
            return nil
        }
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: Int(RN_VIDEO_WIDTH), height: Int(RN_VIDEO_HEIGHT), mipmapped: false)
        td.usage = .shaderRead
        td.storageMode = .shared
        guard let tex = device.makeTexture(descriptor: td) else { return nil }
        texture = tex
    }

    /// Destination rectangle (pixels, origin bottom-left) and overscan crop (pixels hidden on every
    /// side) for a drawable size.
    static func viewport(drawableSize size: CGSize, options: DisplayOptions) -> (CGRect, Int) {
        let crop = options.hideOverscan ? 8 : 0
        let srcH = Double(Int(RN_VIDEO_HEIGHT) - 2 * crop), srcW = Double(Int(RN_VIDEO_WIDTH) - 2 * crop)
        let par = options.pixelAspect87 ? 8.0 / 7.0 : 1.0
        var scale = min(size.width / (srcW * par), size.height / srcH)
        if options.integerScale && scale >= 1 { scale = floor(scale) }
        let dw = (srcW * par * scale).rounded(), dh = (srcH * scale).rounded()
        let x0 = ((size.width - dw) / 2).rounded(), y0 = ((size.height - dh) / 2).rounded()
        return (CGRect(x: x0, y: y0, width: dw, height: dh), crop)
    }

    // What the last present showed.
    private var shownFrame: UInt64 = 0
    private var shownSize = CGSize.zero
    private var shownOptions: DisplayOptions?
    private var shownCRT: CRTSettingsModel.Snapshot?
    private var crtPending = false   // CRT on but its tube plan was not ready: try again shortly

    // GPU-heavy pictures (the CRT model: ~8 ms of GPU per frame at 1080p on M1 Max, more than the
    // direct-to-display path at 120 Hz leaves after the commit) are built one refresh ahead: the passes that
    // make a new picture are committed with the frame, and the next refresh's present shows it.
    // Every picture then reaches the screen exactly one refresh later, instead of missing its
    // refresh and queueing behind it (a sticky backlog that starved the display link of drawables:
    // skipped refreshes, judder, emulation below 60 fps, audio underruns; docs/FRAME_PACING.md).
    // Decided by BuildAhead (DisplayPacing.swift) from the measured picture-building GPU time.
    private var buildAhead = BuildAhead()
    var pipelined: Bool { buildAhead.active }
    /// A new picture that was built for the next present (pipelined).
    private var pending: (meta: FrameMeta, commit: UInt64, gpu: GPUSpan)?
    var hasPendingPicture: Bool { pending != nil }

    /// Display-link pacing (emulation thread): renders into the display link's drawable for the
    /// refresh at `targetPresentation`. `newFrame`: this refresh starts a new emulated frame (just
    /// published); otherwise the current picture is presented again when `repeatPicture` (steady
    /// cadence) or something changed (size, options, CRT). Returns the commit time (mach ticks)
    /// when something was presented (pipelined: the new picture's build commit), nil when nothing
    /// was. `onPresented(meta, commit, seconds, gpu)` is called for the present that first shows a
    /// new frame (on a Metal thread; seconds = 0 when it was never shown); `onRepeatPresented` for
    /// a repeat that was shown. `presentDelay`: targetPresentationTimestamp - targetTimestamp of
    /// the display link update (pipelining decision, BuildAhead).
    /// `skipRepeat`: pipelined, this refresh would only show the current picture again: skip that
    /// present (backlog drain) and only build the new picture.
    @discardableResult
    func present(drawable: CAMetalDrawable, targetPresentation: Double, presentDelay: Double, newFrame: Bool, repeatPicture: Bool,
                 skipRepeat: Bool = false,
                 onPresented: @escaping (FrameMeta, UInt64, Double, GPUSpan) -> Void,
                 onRepeatPresented: @escaping (UInt64, Double) -> Void) -> UInt64? {
        let (fetched, changed) = fetch(drawableSize: drawable.layer.drawableSize)
        var newMeta = newFrame ? fetched : nil
        if !newFrame, let m = fetched, m.emulatedTime == 0 { newMeta = m }  // seek / option refresh: show it
        let crtState = CRTSettingsModel.shared.snapshot
        updatePipelining(crtOn: crtState.enabled, presentDelay: presentDelay)
        if !pipelined, let p = pending {
            // Left the pipelined mode: show the picture that was built (unless a newer one comes).
            pending = nil
            if newMeta == nil { return show(p, drawable: drawable, target: targetPresentation, crtState: crtState, onPresented: onPresented) }
        }
        if pipelined {
            // This drawable shows what was built before; the new picture is built for the next one.
            var shownCommit: UInt64?
            if let p = pending {
                pending = nil
                shownCommit = show(p, drawable: drawable, target: targetPresentation, crtState: crtState, onPresented: onPresented)
            } else if !skipRepeat && (newMeta != nil || changed || repeatPicture) {
                shownCommit = encodeAndPresent(build: nil, drawable: drawable, crtState: crtState, span: nil,
                                               onPresented: { _, _ in }, onRepeatPresented: onRepeatPresented)
            }
            guard var m = newMeta else { return shownCommit }
            let span = GPUSpan()
            m.targetPresentation = 0
            guard let commit = build(m, crtState: crtState, drawableSize: drawable.layer.drawableSize, span: span) else { return shownCommit }
            pending = (m, commit, span)
            return commit
        }
        if newMeta == nil && !changed && !repeatPicture { return nil }
        newMeta?.targetPresentation = targetPresentation
        let span = GPUSpan()
        return encodeAndPresent(build: newMeta, drawable: drawable, crtState: crtState, span: span,
                                onPresented: { commit, t in if let m = newMeta { onPresented(m, commit, t, span) } },
                                onRepeatPresented: onRepeatPresented)
    }

    /// Pipelined: presents the already built picture `p` (first appearance of its frame).
    private func show(_ p: (meta: FrameMeta, commit: UInt64, gpu: GPUSpan), drawable: CAMetalDrawable, target: Double,
                      crtState: CRTSettingsModel.Snapshot, onPresented: @escaping (FrameMeta, UInt64, Double, GPUSpan) -> Void) -> UInt64? {
        var meta = p.meta
        meta.targetPresentation = target
        shownFrame = meta.frame
        let commit = p.commit, span = p.gpu
        return encodeAndPresent(build: nil, revealed: meta, drawable: drawable, crtState: crtState, span: nil,
                                onPresented: { _, t in onPresented(meta, commit, t, span) }, onRepeatPresented: { _, _ in })
    }

    /// Pipelined: encodes and commits the passes that build the picture of `m` (CRT). Returns the
    /// commit time, nil when nothing could be encoded.
    private func build(_ m: FrameMeta, crtState: CRTSettingsModel.Snapshot, drawableSize: CGSize, span: GPUSpan) -> UInt64? {
        guard let crt = crtRenderer(), crtFrame.valid, let cb = queue.makeCommandBuffer() else { return nil }
        configureCRT(crt, crtState: crtState, size: drawableSize)
        crtFrame.encode(into: crt, cb: cb)
        if m.emulatedTime != 0 { latency.recordDraw(refresh: FramePacing.period) }
        addBuildTiming(cb, span: span)
        cb.commit()
        return HostClock.now()
    }

    /// Encodes the picture into `drawable` and presents it. `build`: a new frame whose picture is
    /// built and shown now (not pipelined); `revealed`: a new frame whose picture was built before
    /// (pipelined). Handlers: `onPresented(commit, seconds)` for a new frame's first appearance,
    /// `onRepeatPresented(frame, seconds)` otherwise. Returns the commit time.
    private func encodeAndPresent(build: FrameMeta?, revealed: FrameMeta? = nil, drawable: CAMetalDrawable,
                                  crtState: CRTSettingsModel.Snapshot, span: GPUSpan?,
                                  onPresented: @escaping (UInt64, Double) -> Void,
                                  onRepeatPresented: @escaping (UInt64, Double) -> Void) -> UInt64? {
        guard let cb = queue.makeCommandBuffer() else { return nil }
        if let m = build { shownFrame = m.frame }
        let pictureFrame = shownFrame
        encode(newMeta: build, drawable: drawable, cb: cb, crtState: crtState, span: span)
        let commit = HostClock.now()
        if let m = build ?? revealed, m.emulatedTime != 0 {
            if let span, build != nil { cb.addCompletedHandler { b in span.extend(b.gpuStartTime, b.gpuEndTime) } }
            drawable.addPresentedHandler { d in onPresented(commit, d.presentedTime) }   // 0: never shown (dropped)
        } else {
            drawable.addPresentedHandler { d in
                if d.presentedTime > 0 { onRepeatPresented(pictureFrame, d.presentedTime) }
            }
        }
        cb.present(drawable)
        cb.commit()
        return commit
    }

    private func updatePipelining(crtOn: Bool, presentDelay: Double) {
        if crtOn { buildAhead.update(presentDelay: presentDelay) } else { buildAhead.reset() }
    }

    /// GPU timing of a command buffer that builds a new picture (CRT passes).
    private func addBuildTiming(_ cb: MTLCommandBuffer, span: GPUSpan) {
        let lat = latency, times = buildTimesLock
        cb.addCompletedHandler { [weak self] b in
            guard b.gpuEndTime > b.gpuStartTime else { return }
            span.extend(b.gpuStartTime, b.gpuEndTime)
            let sec = b.gpuEndTime - b.gpuStartTime
            lat.recordDisplayGPU(ms: sec * 1000, crtInfo: self?.crtInfo ?? "")
            times.lock(); self?.buildTimesIn.append(sec); times.unlock()
        }
    }
    // Build times from completion handlers (any thread) -> buildAhead (emulation thread).
    private let buildTimesLock = NSLock()
    private var buildTimesIn: [Double] = []
    private var crtInfo_ = ""
    private var crtInfo: String { buildTimesLock.lock(); defer { buildTimesLock.unlock() }; return crtInfo_ }
    private func drainBuildTimes() {
        buildTimesLock.lock(); let t = buildTimesIn; buildTimesIn.removeAll(keepingCapacity: true); buildTimesLock.unlock()
        for x in t { buildAhead.add(x) }
    }

    /// Takes a newly published frame into the texture (and the CRT store). Returns its meta and
    /// whether anything else that is shown changed since the last draw.
    private func fetch(drawableSize wanted: CGSize) -> (FrameMeta?, Bool) {
        drainBuildTimes()
        let options = self.options
        var newMeta: FrameMeta?
        let w = Int(RN_VIDEO_WIDTH)
        let crtState = CRTSettingsModel.shared.snapshot
        let crtOn = crtState.enabled
        let store = crtFrame
        // CRT just switched on (e.g. while paused): fetch the current frame again for it.
        let refetch = crtOn && !store.valid
        seenSeq = frames.readFrameIfNewer(than: refetch ? .max : seenSeq) { px, codes, meta in
            texture.replace(region: MTLRegionMake2D(0, 0, w, Int(RN_VIDEO_HEIGHT)), mipmapLevel: 0, withBytes: px, bytesPerRow: w * 4)
            newMeta = meta
            if refetch { newMeta?.emulatedTime = 0 }   // not a newly emulated frame: no latency sample
            if crtOn { store.store(px, codes, meta) }
        }
        let pill = pillKey(size: wanted, options: options, crtOn: crtOn).key
        let changed = crtPending || wanted != shownSize || options != shownOptions || crtState != shownCRT || pill != shownPill
        return (newMeta, changed)
    }

    /// Sizes the CRT tube for the drawable; returns its destination rectangle and crop.
    @discardableResult
    private func configureCRT(_ crt: CRTRenderer, crtState: CRTSettingsModel.Snapshot, size: CGSize) -> (CGRect, Double) {
        let (dst, cropFraction) = Self.crtViewport(drawableSize: size, options: options)
        let (tw, th) = CRTRenderer.tubeSize(forDestination: dst.size, cropFraction: cropFraction, maxWidth: Self.crtMaxWidth)
        crt.configure(settings: crtState.settings, outputWidth: tw, outputHeight: th)
        return (dst, cropFraction)
    }

    /// Encodes the picture (plain or CRT) into `drawable` on `cb`. CRT: when `newMeta` is given
    /// (or the tube has no picture yet), the passes that build it are committed first in their own
    /// command buffer (timed: GPUSpan / pipelining decision).
    private func encode(newMeta: FrameMeta?, drawable: CAMetalDrawable, cb: MTLCommandBuffer,
                        crtState: CRTSettingsModel.Snapshot, span: GPUSpan?) {
        let options = self.options
        let crtOn = crtState.enabled
        let store = crtFrame
        if let m = newMeta, m.emulatedTime != 0 { latency.recordDraw(refresh: FramePacing.period) }
        if shownSize != drawable.layer.drawableSize { latency.recordLayerSize(drawable.layer.drawableSize) }
        shownSize = drawable.layer.drawableSize
        shownOptions = options
        shownCRT = crtState
        let size = CGSize(width: drawable.texture.width, height: drawable.texture.height)
        let pill = pillKey(size: size, options: options, crtOn: crtOn)
        shownPill = pill.key
        let rpd = MTLRenderPassDescriptor()
        rpd.colorAttachments[0].texture = drawable.texture
        rpd.colorAttachments[0].loadAction = .clear
        rpd.colorAttachments[0].storeAction = .store
        rpd.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)

        // Physical CRT model (nesterm port).
        var crtShown = false
        if crtOn, let crt = crtRenderer() {
            let (dst, cropFraction) = configureCRT(crt, crtState: crtState, size: size)
            if newMeta != nil || !crt.hasOutput, store.valid, let bcb = queue.makeCommandBuffer() {
                store.encode(into: crt, cb: bcb)
                addBuildTiming(bcb, span: span ?? GPUSpan())
                bcb.commit()
            }
            if crt.hasOutput, let enc = cb.makeRenderCommandEncoder(descriptor: rpd) {
                crt.encodeShow(enc, targetSize: size, dst: dst, cropFraction: cropFraction)
                drawPill(enc, size: size, pill)
                enc.endEncoding()
                crtShown = true
                if let o = crt.outputSize {
                    let info = "\(o.width)x\(o.height) " + (store.usedCodes ? "RF" : "RGB") + (pipelined ? " +1" : "")
                    buildTimesLock.lock(); crtInfo_ = info; buildTimesLock.unlock()
                }
            }
        } else if !crtOn && crtRenderer_ != nil {
            crtRenderer_ = nil          // free the tube buffers when CRT is switched off
            store.valid = false
        }
        crtPending = crtOn && !crtShown
        if !crtShown, let enc = cb.makeRenderCommandEncoder(descriptor: rpd) {
            let (r, crop) = Self.viewport(drawableSize: size, options: options)
            let srcH = Double(Int(RN_VIDEO_HEIGHT) - 2 * crop), srcW = Double(Int(RN_VIDEO_WIDTH) - 2 * crop)
            var rect = SIMD4<Float>(Float(r.minX / size.width * 2 - 1), Float(r.minY / size.height * 2 - 1),
                                    Float(r.width / size.width * 2), Float(r.height / size.height * 2))
            var uvr = SIMD4<Float>(Float(Double(crop) / Double(RN_VIDEO_WIDTH)), Float(Double(crop) / Double(RN_VIDEO_HEIGHT)),
                                   Float(srcW / Double(RN_VIDEO_WIDTH)), Float(srcH / Double(RN_VIDEO_HEIGHT)))
            enc.setRenderPipelineState(pipeline)
            enc.setVertexBytes(&rect, length: MemoryLayout<SIMD4<Float>>.size, index: 0)
            enc.setVertexBytes(&uvr, length: MemoryLayout<SIMD4<Float>>.size, index: 1)
            enc.setFragmentTexture(texture, index: 0)
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            drawPill(enc, size: size, pill)
            enc.endEncoding()
        }
        if newMeta != nil && !crtOn {
            let lat = latency
            cb.addCompletedHandler { b in
                guard b.gpuEndTime > b.gpuStartTime else { return }
                lat.recordDisplayGPU(ms: (b.gpuEndTime - b.gpuStartTime) * 1000, crtInfo: "")
            }
        }
    }

    // MARK: menu pill (MenuPill.swift)

    /// What the pill looks like on a present (nil-equivalent: not visible).
    private struct PillKey: Equatable {
        var generation = -1
        var alpha = 0          // twentieths
        var rect = CGRect.zero
    }
    private var shownPill: PillKey?
    private var pillTexture: MTLTexture?
    private var pillTextureGeneration = -1

    /// The picture's rectangle (pixels, origin top-left) that the pill may lie over.
    private static func pictureRect(size: CGSize, options: DisplayOptions, crtOn: Bool) -> CGRect {
        if crtOn { return crtViewport(drawableSize: size, options: options).0 }
        let r = viewport(drawableSize: size, options: options).0
        return CGRect(x: r.minX, y: size.height - r.maxY, width: r.width, height: r.height)
    }

    private func pillKey(size: CGSize, options: DisplayOptions, crtOn: Bool) -> (key: PillKey, state: MenuPillOverlay.State) {
        let st = MenuPillOverlay.shared.state
        guard st.visible, st.image != nil else { return (PillKey(), st) }
        let r = MenuPillLayout.rect(areaSize: size, pillSize: st.pixelSize, scale: st.scale)
        let over = MenuPillLayout.overPicture(r, picture: Self.pictureRect(size: size, options: options, crtOn: crtOn))
        let a = MenuPillLayout.alpha(elapsed: CACurrentMediaTime() - st.activity, overPicture: over)
        return (PillKey(generation: st.generation, alpha: Int((a * 20).rounded()), rect: r), st)
    }

    /// Draws the pill into the current pass (premultiplied alpha blend): a few µs of GPU.
    private func drawPill(_ enc: MTLRenderCommandEncoder, size: CGSize, _ pill: (key: PillKey, state: MenuPillOverlay.State)) {
        let k = pill.key
        guard k.generation >= 0, k.alpha > 0, let tex = pillTexture(pill.state) else { return }
        let r = k.rect
        var rect = SIMD4<Float>(Float(r.minX / size.width * 2 - 1), Float((size.height - r.maxY) / size.height * 2 - 1),
                                Float(r.width / size.width * 2), Float(r.height / size.height * 2))
        var uvr = SIMD4<Float>(0, 0, 1, 1)
        var alpha = Float(k.alpha) / 20
        enc.setRenderPipelineState(pillPipeline)
        enc.setVertexBytes(&rect, length: MemoryLayout<SIMD4<Float>>.size, index: 0)
        enc.setVertexBytes(&uvr, length: MemoryLayout<SIMD4<Float>>.size, index: 1)
        enc.setFragmentTexture(tex, index: 0)
        enc.setFragmentBytes(&alpha, length: MemoryLayout<Float>.size, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }

    /// The pill picture as a texture (made again when MenuPillOverlay renders a new one).
    private func pillTexture(_ st: MenuPillOverlay.State) -> MTLTexture? {
        if pillTextureGeneration == st.generation, let t = pillTexture { return t }
        guard let img = st.image else { return nil }
        let w = img.width, h = img.height
        var bytes = [UInt8](repeating: 0, count: w * h * 4)
        let ok = bytes.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                      space: CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
            else { return false }
            ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        guard ok else { return nil }
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
        td.usage = .shaderRead
        td.storageMode = .shared
        guard let t = device.makeTexture(descriptor: td) else { return nil }
        t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: bytes, bytesPerRow: w * 4)
        pillTexture = t
        pillTextureGeneration = st.generation
        return t
    }

    // MARK: CRT

    /// nesterm OUTPUT-RESOLUTION-SPEC: the tube follows the displayed pixels, bounded at 1600x1200
    /// (the reference's detector work budget); larger destinations are scaled up in linear light.
    static let crtMaxWidth = 1600

    private var crtRenderer_: CRTRenderer?
    private var crtFailed = false
    private let crtFrame = CRTFrameStore()

    private func crtRenderer() -> CRTRenderer? {
        if let r = crtRenderer_ { return r }
        if crtFailed { return nil }
        do { crtRenderer_ = try CRTRenderer(device: device, targetPixelFormat: .bgra8Unorm) } catch {
            NSLog("ReplayNES: CRT pipeline unavailable: \(error)")
            crtFailed = true
        }
        return crtRenderer_
    }

    /// CRT destination: the full 4:3 raster (minus the hidden overscan rows), as tall as the plain
    /// viewport would be (integer scale or FILL), centred. Pixel aspect does not apply (the tube
    /// geometry is 4:3 by construction). Origin top-left (Metal render target coordinates).
    static func crtViewport(drawableSize size: CGSize, options: DisplayOptions) -> (CGRect, Double) {
        let cropFraction = (options.hideOverscan ? 8.0 : 0.0) / 240
        let aspect = (4.0 / 3.0) / (1 - 2 * cropFraction)
        var plain = options
        plain.pixelAspect87 = false
        let (r, _) = viewport(drawableSize: size, options: plain)
        var h = r.height, wd = (h * aspect).rounded()
        if wd > size.width { wd = size.width; h = (wd / aspect).rounded() }
        let x0 = ((size.width - wd) / 2).rounded(), y0 = ((size.height - h) / 2).rounded()
        return (CGRect(x: x0, y: y0, width: wd, height: h), cropFraction)
    }
}

/// Last frame handed to the CRT (re-encoded when the tube plan changes while paused).
final class CRTFrameStore {
    var valid = false
    private(set) var usedCodes = false
    private var pixels = [UInt32](repeating: 0xFF00_0000, count: Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT))
    private var codes = [UInt16](repeating: 0x0F, count: Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT))
    private var meta = FrameMeta()

    func store(_ px: UnsafePointer<UInt32>, _ c: UnsafePointer<UInt16>?, _ m: FrameMeta) {
        pixels.withUnsafeMutableBufferPointer { $0.baseAddress!.update(from: px, count: $0.count) }
        if let c { codes.withUnsafeMutableBufferPointer { $0.baseAddress!.update(from: c, count: $0.count) } }
        meta = m
        meta.hasCodes = c != nil && m.hasCodes
        valid = true
    }

    /// RF path from the raw PPU codes; the flash-filtered RGB picture (nesterm's synthetic-RGB
    /// source) when the photosensitive filter altered the frame or the core has no codes.
    func encode(into crt: CRTRenderer, cb: MTLCommandBuffer) {
        usedCodes = meta.hasCodes && !meta.flashAltered
        let ordinal = meta.hasCodes ? meta.signalFrame : meta.frame
        if usedCodes {
            codes.withUnsafeBufferPointer { _ = crt.encode(.codes($0.baseAddress!, burstPhase: meta.burstPhase), ordinal: ordinal, into: cb) }
        } else {
            pixels.withUnsafeBufferPointer { _ = crt.encode(.rgb($0.baseAddress!), ordinal: ordinal, into: cb) }
        }
    }
}

/// The game viewport: a layer-backed view whose CAMetalLayer is driven by the emulation thread's
/// display link.
final class GameLayerView: NSView {
    let renderer: GameRenderer?
    private let metalLayer = CAMetalLayer()
    private weak var emu: EmulationController?
    private var displayTarget: DisplayTarget?

    init(emu: EmulationController) {
        self.emu = emu
        renderer = GameRenderer(frames: emu.frames, latency: emu.latency)
        super.init(frame: NSRect(x: 0, y: 0, width: 512, height: 480))
        metalLayer.device = renderer?.device
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
        // Opaque, no transform, no overlapping views while playing in full screen: the layer can
        // then be shown direct-to-display (no compositor pass; docs/FRAME_PACING.md).
        metalLayer.isOpaque = true
        metalLayer.backgroundColor = CGColor(gray: 0, alpha: 1)
        metalLayer.maximumDrawableCount = 3
        metalLayer.displaySyncEnabled = true
        metalLayer.presentsWithTransaction = false
        wantsLayer = true
        layerContentsRedrawPolicy = .never
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    override func makeBackingLayer() -> CALayer { metalLayer }

    /// A click on the Metal-drawn menu pill (MenuPill.swift).
    var onPillClick: (() -> Void)?

    override func mouseDown(with event: NSEvent) {
        let st = MenuPillOverlay.shared.state
        if st.visible, st.image != nil, let onPillClick {
            let p = convert(event.locationInWindow, from: nil)
            let scale = window?.backingScaleFactor ?? 2
            let px = CGPoint(x: p.x * scale, y: (bounds.height - p.y) * scale)
            let r = MenuPillLayout.rect(areaSize: metalLayer.drawableSize, pillSize: st.pixelSize, scale: st.scale)
            if r.insetBy(dx: -4 * scale, dy: -4 * scale).contains(px) { onPillClick(); return }
        }
        super.mouseDown(with: event)
    }
    override var isOpaque: Bool { true }
    override var wantsUpdateLayer: Bool { true }
    override func updateLayer() {}  // drawn by the emulation thread

    private var screenObserver: NSObjectProtocol?

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        updateDrawableSize()
        if let o = screenObserver { NotificationCenter.default.removeObserver(o); screenObserver = nil }
        if let w = window {
            // Another display (refresh rate, fixed or variable refresh): a new display link for it.
            screenObserver = NotificationCenter.default.addObserver(forName: NSWindow.didChangeScreenNotification, object: w, queue: .main) { [weak self] _ in
                guard let self, self.displayTarget != nil else { return }
                self.stopRendering()
                self.startRendering()
            }
            startRendering()
        } else {
            stopRendering()
        }
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        updateDrawableSize()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        updateDrawableSize()
    }

    /// Main thread. The render path takes its geometry from each drawable's texture.
    private func updateDrawableSize() {
        let scale = window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2
        let size = CGSize(width: max(1, (bounds.width * scale).rounded()), height: max(1, (bounds.height * scale).rounded()))
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        metalLayer.contentsScale = scale
        if metalLayer.drawableSize != size { metalLayer.drawableSize = size }
        CATransaction.commit()
    }

    /// Display options changed (main thread); shown from the next refresh.
    func setOptions(_ o: DisplayOptions) {
        guard let renderer, renderer.options != o else { return }
        renderer.options = o
    }

    private func startRendering() {
        guard let renderer, let emu, displayTarget == nil else { return }
        let screen = window?.screen ?? NSScreen.main
        let t = DisplayTarget(layer: metalLayer, renderer: renderer, maxFPS: screen?.maximumFramesPerSecond ?? 60,
                              variableRefresh: screen.map { $0.maximumRefreshInterval - $0.minimumRefreshInterval > 0.0005 } ?? true)
        displayTarget = t
        emu.setDisplayTarget(t)
    }

    func stopRendering() {
        if let t = displayTarget { emu?.clearDisplayTarget(t) }
        displayTarget = nil
    }
}

struct MetalGameView: NSViewRepresentable {
    let emu: EmulationController
    var options: DisplayOptions
    var onPillClick: (() -> Void)?

    func makeNSView(context: Context) -> GameLayerView {
        let v = GameLayerView(emu: emu)
        v.setOptions(options)
        v.onPillClick = onPillClick
        return v
    }

    func updateNSView(_ nsView: GameLayerView, context: Context) {
        nsView.setOptions(options)
        nsView.onPillClick = onPillClick
    }

    static func dismantleNSView(_ nsView: GameLayerView, coordinator: ()) {
        nsView.stopRendering()
    }
}
