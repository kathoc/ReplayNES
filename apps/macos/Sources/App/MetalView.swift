// Metal game viewport: 256x240 BGRA texture, nearest-neighbour, optional integer scale,
// 8:7 pixel aspect and overscan hiding, or the physical CRT model (CRTRenderer, nesterm port).
// Presentation only: frame drops/dupes here never affect emulation.
// Each emulated frame is rendered by a dedicated real-time present thread as soon as it is
// published (never on the main thread, so SwiftUI / AppKit work cannot delay it) and scheduled to
// appear at its tick's time plus an adaptive lead (PresentLead), so every frame stays on screen
// equally long: 60 fps content on a 120 Hz ProMotion display shows each frame for two refreshes.
// SPDX-License-Identifier: GPL-2.0-or-later
import Metal
import QuartzCore
import SwiftUI

struct DisplayOptions: Equatable {
    var integerScale = true
    var pixelAspect87 = false
    var hideOverscan = true
}

/// Present thread only, except `options` (any thread).
final class GameRenderer {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
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

    // What the last present showed (present thread).
    private var shownSize = CGSize.zero
    private var shownOptions: DisplayOptions?
    private var shownCRT: CRTSettingsModel.Snapshot?
    private var crtPending = false   // CRT on but its tube plan was not ready: try again shortly

    // Present scheduling (PresentLead), fed by the presented handlers (any thread).
    private let leadLock = NSLock()
    private var leadControl = PresentLead()
    private var lastPresented: (frame: UInt64, time: Double)?
    private var presentLead: Double { leadLock.lock(); defer { leadLock.unlock() }; return leadControl.lead }

    private func presented(frame: UInt64, at t: Double) {
        leadLock.lock()
        if let l = lastPresented, frame == l.frame + 1, t > l.time, t - l.time < FramePacing.continuityLimit {
            leadControl.observe(interval: t - l.time)
        }
        lastPresented = (frame, t)
        let lead = leadControl.lead
        leadLock.unlock()
        latency.recordPresentLead(lead)
    }

    /// Present thread: shows what is new (an emulated frame, a size / option / CRT change);
    /// otherwise nothing is drawn and the layer keeps its picture.
    func draw(layer: CAMetalLayer) {
        let options = self.options
        var newMeta: FrameMeta?
        let w = Int(RN_VIDEO_WIDTH)
        let crtState = CRTSettingsModel.shared.snapshot
        let crtOn = crtState.enabled
        let wanted = layer.drawableSize
        let store = crtFrame
        // CRT just switched on (e.g. while paused): fetch the current frame again for it.
        let refetch = crtOn && !store.valid
        seenSeq = frames.readFrameIfNewer(than: refetch ? .max : seenSeq) { px, codes, meta in
            texture.replace(region: MTLRegionMake2D(0, 0, w, Int(RN_VIDEO_HEIGHT)), mipmapLevel: 0, withBytes: px, bytesPerRow: w * 4)
            newMeta = meta
            if refetch { newMeta?.emulatedTime = 0 }   // not a newly emulated frame: no latency sample
            if crtOn { store.store(px, codes, meta) }
        }
        guard newMeta != nil || crtPending || wanted != shownSize || options != shownOptions || crtState != shownCRT else { return }
        guard let drawable = layer.nextDrawable(), let cb = queue.makeCommandBuffer() else { shownOptions = nil; return }
        if let m = newMeta, m.emulatedTime != 0 { latency.recordDraw(refresh: FramePacing.period) }
        shownSize = wanted
        shownOptions = options
        shownCRT = crtState
        let size = CGSize(width: drawable.texture.width, height: drawable.texture.height)
        let rpd = MTLRenderPassDescriptor()
        rpd.colorAttachments[0].texture = drawable.texture
        rpd.colorAttachments[0].loadAction = .clear
        rpd.colorAttachments[0].storeAction = .store
        rpd.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)

        // Physical CRT model (nesterm port): every pass is encoded into THIS command buffer, so a
        // new emulated frame is presented exactly like on the plain path.
        var crtShown = false, crtInfo = ""
        if crtOn, let crt = crtRenderer() {
            let (dst, cropFraction) = Self.crtViewport(drawableSize: size, options: options)
            let (tw, th) = CRTRenderer.tubeSize(forDestination: dst.size, cropFraction: cropFraction, maxWidth: Self.crtMaxWidth)
            crt.configure(settings: crtState.settings, outputWidth: tw, outputHeight: th)
            if newMeta != nil || !crt.hasOutput, store.valid {
                store.encode(into: crt, cb: cb)
            }
            if crt.hasOutput, let enc = cb.makeRenderCommandEncoder(descriptor: rpd) {
                crt.encodeShow(enc, targetSize: size, dst: dst, cropFraction: cropFraction)
                enc.endEncoding()
                crtShown = true
                if let o = crt.outputSize { crtInfo = "\(o.width)x\(o.height) " + (store.usedCodes ? "RF" : "RGB") }
            }
        } else if !crtOn && crtRenderer_ != nil {
            crtRenderer_ = nil          // free the tube buffers when CRT is switched off
            store.valid = false
        }
        crtPending = crtOn && !crtShown
        if !crtShown {
            guard let enc = cb.makeRenderCommandEncoder(descriptor: rpd) else { return }
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
            enc.endEncoding()
        }
        if let meta = newMeta {
            let lat = latency
            if meta.emulatedTime != 0 {
                drawable.addPresentedHandler { [weak self] d in
                    guard d.presentedTime > 0 else { return }
                    lat.recordPresent(meta: meta, presentedSeconds: d.presentedTime)
                    if meta.deadline != 0 { self?.presented(frame: meta.frame, at: d.presentedTime) }
                }
            }
            let info = crtInfo
            cb.addCompletedHandler { b in
                if b.gpuEndTime > b.gpuStartTime { lat.recordDisplayGPU(ms: (b.gpuEndTime - b.gpuStartTime) * 1000, crtInfo: info) }
            }
        }
        // On the tick grid plus the lead; frames not made by a paced tick (seeks while paused,
        // option changes) and frames already past their slot go out at once.
        let target = newMeta.map { $0.deadline != 0 ? HostClock.seconds($0.deadline) + presentLead : 0 } ?? 0
        if target > HostClock.seconds(HostClock.now()) {
            cb.present(drawable, atTime: target)
        } else {
            cb.present(drawable)
        }
        cb.commit()
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

/// The game viewport: a layer-backed view whose CAMetalLayer is drawn by its PresentThread.
final class GameLayerView: NSView {
    let renderer: GameRenderer?
    private let metalLayer = CAMetalLayer()
    private var presenter: PresentThread?

    init(frames: FrameBuffer, latency: LatencyMeter) {
        renderer = GameRenderer(frames: frames, latency: latency)
        super.init(frame: NSRect(x: 0, y: 0, width: 512, height: 480))
        metalLayer.device = renderer?.device
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
        metalLayer.isOpaque = true
        metalLayer.backgroundColor = CGColor(gray: 0, alpha: 1)
        // One drawable on screen, up to two scheduled ahead (present(atTime:)).
        metalLayer.maximumDrawableCount = 3
        metalLayer.displaySyncEnabled = true
        wantsLayer = true
        layerContentsRedrawPolicy = .never
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    deinit { presenter?.stop() }

    override func makeBackingLayer() -> CALayer { metalLayer }
    override var isOpaque: Bool { true }
    override var wantsUpdateLayer: Bool { true }
    override func updateLayer() {}  // the present thread draws the contents

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        updateDrawableSize()
        if window != nil { startRendering() } else { stopRendering() }
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        updateDrawableSize()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        updateDrawableSize()
    }

    /// Main thread. The present thread takes its geometry from each drawable's texture.
    private func updateDrawableSize() {
        let scale = window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2
        let size = CGSize(width: max(1, (bounds.width * scale).rounded()), height: max(1, (bounds.height * scale).rounded()))
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        metalLayer.contentsScale = scale
        if metalLayer.drawableSize != size { metalLayer.drawableSize = size }
        CATransaction.commit()
        presenter?.wake()
    }

    /// Display options changed (main thread).
    func setOptions(_ o: DisplayOptions) {
        guard let renderer, renderer.options != o else { return }
        renderer.options = o
        presenter?.wake()
    }

    private func startRendering() {
        guard presenter == nil, let renderer else { return }
        presenter = PresentThread(renderer: renderer, layer: metalLayer)
    }

    func stopRendering() {
        presenter?.stop()
        presenter = nil
    }
}

/// Real-time thread that renders and presents each new emulated frame as soon as it is published
/// (woken by FrameBuffer), plus size / option changes; it sleeps otherwise.
final class PresentThread {
    private let wakeup = DispatchSemaphore(value: 0)
    private let frames: FrameBuffer
    private let lock = NSLock()
    private var running = true

    init(renderer: GameRenderer, layer: CAMetalLayer) {
        frames = renderer.frames
        let wakeup = self.wakeup
        let t = Thread {  // retains self until the loop ends (stop())
            // Woken once per emulated frame; ~0.3 ms of CPU each.
            HostClock.makeCurrentThreadRealtime(period: FramePacing.period, computation: 0.002, constraint: 0.008)
            while self.isRunning {
                // The timeout re-checks a CRT tube plan still being built while nothing new arrives.
                _ = wakeup.wait(timeout: .now() + 0.1)
                guard self.isRunning else { break }
                autoreleasepool { renderer.draw(layer: layer) }  // drawables are autoreleased
            }
        }
        t.name = "ReplayNES.present"
        t.qualityOfService = .userInteractive
        frames.addPublishObserver(wakeup)
        t.start()
    }

    private var isRunning: Bool { lock.lock(); defer { lock.unlock() }; return running }

    func wake() { wakeup.signal() }

    func stop() {
        frames.removePublishObserver(wakeup)
        lock.lock(); running = false; lock.unlock()
        wakeup.signal()
    }
}

struct MetalGameView: NSViewRepresentable {
    let emu: EmulationController
    var options: DisplayOptions

    func makeNSView(context: Context) -> GameLayerView {
        let v = GameLayerView(frames: emu.frames, latency: emu.latency)
        v.setOptions(options)
        return v
    }

    func updateNSView(_ nsView: GameLayerView, context: Context) {
        nsView.setOptions(options)
    }

    static func dismantleNSView(_ nsView: GameLayerView, coordinator: ()) {
        nsView.stopRendering()
    }
}
