// Metal game viewport: 256x240 BGRA texture, nearest-neighbour, optional integer scale,
// 8:7 pixel aspect and overscan hiding. Presentation only: frame drops/dupes here never
// affect emulation.
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

    /// Destination rectangle (pixels, origin bottom-left) and overscan crop for a drawable size.
    static func viewport(drawableSize size: CGSize, options: DisplayOptions) -> (CGRect, Int) {
        let crop = options.hideOverscan ? 8 : 0
        let srcH = Double(Int(RN_VIDEO_HEIGHT) - 2 * crop), srcW = Double(RN_VIDEO_WIDTH)
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
        seenSeq = frames.readIfNewer(than: seenSeq) { px, meta in
            texture.replace(region: MTLRegionMake2D(0, 0, w, Int(RN_VIDEO_HEIGHT)), mipmapLevel: 0, withBytes: px, bytesPerRow: w * 4)
            newMeta = meta
        }
        guard let rpd = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
              let cb = queue.makeCommandBuffer(), let enc = cb.makeRenderCommandEncoder(descriptor: rpd) else { return }

        let size = view.drawableSize
        let (r, crop) = Self.viewport(drawableSize: size, options: options)
        let srcH = Double(Int(RN_VIDEO_HEIGHT) - 2 * crop)
        var rect = SIMD4<Float>(Float(r.minX / size.width * 2 - 1), Float(r.minY / size.height * 2 - 1),
                                Float(r.width / size.width * 2), Float(r.height / size.height * 2))
        var uvr = SIMD4<Float>(0, Float(Double(crop) / Double(RN_VIDEO_HEIGHT)), 1, Float(srcH / Double(RN_VIDEO_HEIGHT)))
        enc.setRenderPipelineState(pipeline)
        enc.setVertexBytes(&rect, length: MemoryLayout<SIMD4<Float>>.size, index: 0)
        enc.setVertexBytes(&uvr, length: MemoryLayout<SIMD4<Float>>.size, index: 1)
        enc.setFragmentTexture(texture, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding()
        if let meta = newMeta, meta.emulatedTime != 0 {
            let lat = latency
            drawable.addPresentedHandler { d in
                if d.presentedTime > 0 { lat.recordPresent(meta: meta, presentedSeconds: d.presentedTime) }
            }
        }
        cb.present(drawable)
        cb.commit()
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
