// Metal game viewport: 256x240 BGRA texture, nearest-neighbour, optional integer scale,
// 8:7 pixel aspect and overscan hiding, or the physical CRT model (CRTRenderer, nesterm port).
// Presentation only: frame drops/dupes here never affect emulation.
// SPDX-License-Identifier: GPL-2.0-or-later
import MetalKit
import SwiftUI

struct DisplayOptions: Equatable {
    var integerScale = true
    var pixelAspect87 = false
    var hideOverscan = true
}

final class GameRenderer: NSObject, MTKViewDelegate {
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let texture: MTLTexture
    private let frames: FrameBuffer
    private let latency: LatencyMeter
    private var seenSeq: UInt64 = .max
    var options = DisplayOptions()

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

    init?(view: MTKView, frames: FrameBuffer, latency: LatencyMeter) {
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
        super.init()
        view.device = device
        view.colorPixelFormat = .bgra8Unorm
        view.framebufferOnly = true
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        view.preferredFramesPerSecond = 120
        view.enableSetNeedsDisplay = false
        view.isPaused = false
        view.delegate = self
        if let layer = view.layer as? CAMetalLayer {
            // Double buffering: one drawable on screen, one being drawn. Lower latency than the
            // default triple buffering; MTKView still paces on the display link.
            layer.maximumDrawableCount = 2
            layer.displaySyncEnabled = true
        }
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

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

    func draw(in view: MTKView) {
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
        guard let rpd = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
              let cb = queue.makeCommandBuffer() else { return }
        let size = view.drawableSize

        // Physical CRT model (nesterm port): every pass is encoded into THIS command buffer right
        // before the present, so a new emulated frame reaches the next vsync like the plain path.
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
                drawable.addPresentedHandler { d in
                    if d.presentedTime > 0 { lat.recordPresent(meta: meta, presentedSeconds: d.presentedTime) }
                }
            }
            let info = crtInfo
            cb.addCompletedHandler { b in
                if b.gpuEndTime > b.gpuStartTime { lat.recordDisplayGPU(ms: (b.gpuEndTime - b.gpuStartTime) * 1000, crtInfo: info) }
            }
        }
        cb.present(drawable)
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

struct MetalGameView: NSViewRepresentable {
    let emu: EmulationController
    var options: DisplayOptions

    final class Coordinator { var renderer: GameRenderer? }
    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> MTKView {
        let v = MTKView(frame: NSRect(x: 0, y: 0, width: 512, height: 480))
        context.coordinator.renderer = GameRenderer(view: v, frames: emu.frames, latency: emu.latency)
        context.coordinator.renderer?.options = options
        return v
    }

    func updateNSView(_ nsView: MTKView, context: Context) {
        context.coordinator.renderer?.options = options
    }
}
