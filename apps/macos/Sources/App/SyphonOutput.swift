// Streaming output for OBS & co. via Syphon (OBS on macOS: "Syphon Client" source).
// Publishes the game frame as displayed (after the flash reduction filter, before any UI) from
// its own queue: it polls the shared FrameBuffer exactly like the on-screen renderer, so the
// emulation thread is never waited on and a slow consumer only drops output frames.
// Off unless enabled (settings, menu, or the --syphon launch argument for this run only).
// SPDX-License-Identifier: GPL-2.0-or-later
import Metal
import Syphon
import SwiftUI

/// Renders FrameBuffer frames nearest-neighbour into a canvas texture and hands it to a
/// SyphonMetalServer. All state lives on `work`.
final class SyphonPublisher {
    static let serverName = "ReplayNES"

    private let frames: FrameBuffer
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let server: SyphonMetalServer
    private let work = DispatchQueue(label: "io.github.replaynes.syphon", qos: .userInteractive)
    private var timer: DispatchSourceTimer?
    /// Upload ring: with at most `maxInFlight` command buffers pending, the texture written next
    /// is never one the GPU may still be reading.
    private let sources: [MTLTexture]
    private var sourceIndex = 0
    private var inFlight = 0
    private static let maxInFlight = 2
    private var target: MTLTexture?
    private var layout = StreamOutputLayout(size: .default, par87: false)
    private var seenSeq: UInt64 = .max

    private static let shader = """
    #include <metal_stdlib>
    using namespace metal;
    struct VOut { float4 pos [[position]]; float2 uv; };
    vertex VOut vmain(uint vid [[vertex_id]]) {
        float2 c = float2(float(vid & 1), float(vid >> 1));
        VOut o;
        o.pos = float4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
        o.uv = c;
        return o;
    }
    fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {
        constexpr sampler s(filter::nearest, address::clamp_to_edge);
        return float4(tex.sample(s, in.uv).rgb, 1.0);
    }
    """

    /// Throws (with a user-facing reason) when Metal or the Syphon server is unavailable.
    init(frames: FrameBuffer) throws {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
            throw SyphonOutputError("Metal を利用できません")
        }
        let lib = try device.makeLibrary(source: Self.shader, options: nil)
        let d = MTLRenderPipelineDescriptor()
        d.vertexFunction = lib.makeFunction(name: "vmain")
        d.fragmentFunction = lib.makeFunction(name: "fmain")
        d.colorAttachments[0].pixelFormat = .bgra8Unorm
        pipeline = try device.makeRenderPipelineState(descriptor: d)
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: StreamOutputLayout.frameWidth,
                                                          height: StreamOutputLayout.frameHeight, mipmapped: false)
        td.usage = .shaderRead
        td.storageMode = .shared
        sources = try (0..<(Self.maxInFlight + 1)).map { _ in
            guard let t = device.makeTexture(descriptor: td) else { throw SyphonOutputError("テクスチャを作成できません") }
            return t
        }
        let server = SyphonMetalServer(name: Self.serverName, device: device, options: nil)
        // Imported as non-failable, but the Objective-C initializer returns nil on failure.
        guard unsafeBitCast(server, to: UInt.self) != 0 else { throw SyphonOutputError("Syphon サーバーを開始できません") }
        self.frames = frames
        self.device = device
        self.queue = queue
        self.server = server
    }

    func start(size: StreamOutputSize, par87: Bool) {
        configure(size: size, par87: par87)
        work.async { [self] in
            let t = DispatchSource.makeTimerSource(queue: work)
            // ~4 ms polling: frames arrive at ~60 Hz; this bounds the added latency, not the rate.
            t.schedule(deadline: .now(), repeating: .milliseconds(4), leeway: .milliseconds(1))
            t.setEventHandler { [weak self] in self?.tick() }
            timer = t
            t.resume()
        }
    }

    func configure(size: StreamOutputSize, par87: Bool) {
        work.async { [self] in
            let l = StreamOutputLayout(size: size, par87: par87)
            guard l != layout else { return }
            layout = l
            seenSeq = .max   // republish the current frame at the new size (also when paused)
        }
    }

    /// Stops publishing and retires the server (clients see it disappear). Synchronous.
    func stop() {
        work.sync {
            timer?.cancel()
            timer = nil
            server.stop()
        }
    }

    private func tick() {
        // Nobody watching: do nothing and leave the frame unconsumed, so a client that connects
        // later (even while paused) gets the current frame on the next tick.
        guard timer != nil, inFlight < Self.maxInFlight, server.hasClients else { return }
        let src = sources[sourceIndex]
        var got = false
        let seq = frames.readIfNewer(than: seenSeq) { px, _ in
            src.replace(region: MTLRegionMake2D(0, 0, StreamOutputLayout.frameWidth, StreamOutputLayout.frameHeight),
                        mipmapLevel: 0, withBytes: px, bytesPerRow: StreamOutputLayout.frameWidth * 4)
            got = true
        }
        guard got else { return }
        seenSeq = seq
        sourceIndex = (sourceIndex + 1) % sources.count

        let c = layout.canvas
        if target?.width != c.width || target?.height != c.height {
            let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: c.width, height: c.height, mipmapped: false)
            td.usage = [.renderTarget, .shaderRead]
            td.storageMode = .private
            target = device.makeTexture(descriptor: td)
        }
        guard let target, let cb = queue.makeCommandBuffer() else { return }
        let rpd = MTLRenderPassDescriptor()
        rpd.colorAttachments[0].texture = target
        rpd.colorAttachments[0].loadAction = .clear
        rpd.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        rpd.colorAttachments[0].storeAction = .store
        guard let enc = cb.makeRenderCommandEncoder(descriptor: rpd) else { return }
        let p = layout.picture
        enc.setViewport(MTLViewport(originX: p.minX, originY: p.minY, width: p.width, height: p.height, znear: 0, zfar: 1))
        enc.setRenderPipelineState(pipeline)
        enc.setFragmentTexture(src, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding()
        // Top-left origin texture => not flipped. Same pixel format, not framebuffer-only, so
        // Syphon copies it into its shared IOSurface with a blit and publishes on completion.
        server.publishFrameTexture(target, on: cb, imageRegion: NSRect(x: 0, y: 0, width: c.width, height: c.height), flipped: false)
        inFlight += 1
        cb.addCompletedHandler { [weak self] _ in
            guard let self else { return }
            self.work.async { self.inFlight -= 1 }
        }
        cb.commit()
    }
}

struct SyphonOutputError: LocalizedError {
    let message: String
    init(_ m: String) { message = m }
    var errorDescription: String? { message }
}

/// Settings + lifecycle of the streaming output. Main thread only.
final class StreamOutputModel: ObservableObject {
    static let shared = StreamOutputModel()

    @AppStorage("syphonEnabled") private var storedEnabled = false
    @AppStorage("syphonSize") var sizeRaw = StreamOutputSize.default.rawValue { didSet { apply(); objectWillChange.send() } }
    @AppStorage("syphonPAR87") var par87 = false { didSet { apply(); objectWillChange.send() } }

    /// Publishing right now.
    @Published private(set) var active = false
    @Published private(set) var failure: String?

    private var launchOverride = false   // --syphon: on for this run, not saved
    private var frames: FrameBuffer?
    private var publisher: SyphonPublisher?

    var size: StreamOutputSize { StreamOutputSize(rawValue: sizeRaw) ?? .default }
    var isOn: Bool { storedEnabled || launchOverride }

    func setOn(_ on: Bool) {
        storedEnabled = on
        if !on { launchOverride = false }
        objectWillChange.send()
        apply()
    }

    func toggle() { setOn(!isOn) }

    func start(frames: FrameBuffer, forceEnable: Bool) {
        self.frames = frames
        launchOverride = forceEnable
        apply()
    }

    func stop() {
        publisher?.stop()
        publisher = nil
        active = false
    }

    private func apply() {
        guard let frames else { return }
        if !isOn { stop(); failure = nil; return }
        if let publisher {
            publisher.configure(size: size, par87: par87)
            return
        }
        do {
            let p = try SyphonPublisher(frames: frames)
            p.start(size: size, par87: par87)
            publisher = p
            active = true
            failure = nil
        } catch {
            NSLog("ReplayNES: Syphon output failed: \(error)")
            failure = error.localizedDescription
            active = false
        }
    }
}

// MARK: UI hooks

struct StreamOutputCommands: Commands {
    @ObservedObject var stream: StreamOutputModel
    var body: some Commands {
        CommandGroup(after: .sidebar) {
            Button(stream.isOn ? "配信出力 (Syphon) を停止" : "配信出力 (Syphon) を開始") { stream.toggle() }
        }
    }
}

/// Settings section (表示・音声 tab).
struct StreamOutputSection: View {
    @ObservedObject var stream = StreamOutputModel.shared
    var body: some View {
        Section("配信出力 (Syphon)") {
            Toggle("配信出力 (Syphon)", isOn: Binding(get: { stream.isOn }, set: { stream.setOn($0) }))
            Picker("出力サイズ", selection: $stream.sizeRaw) {
                ForEach(StreamOutputSize.allCases) { Text($0.label(par87: stream.par87)).tag($0.rawValue) }
            }
            Toggle("ピクセル比 8:7", isOn: $stream.par87)
            if let f = stream.failure {
                Text("開始できませんでした: \(f)").font(.caption).foregroundStyle(.red)
            } else if stream.active {
                Text("Syphon出力中（サーバー名「\(SyphonPublisher.serverName)」）").font(.caption).foregroundStyle(.green)
            }
            Text("OBS の「Syphon クライアント」ソースで「ReplayNES」を選ぶと、ゲーム画面だけ（フラッシュ低減後・UI なし・オーバースキャン込みの 256×240 全体）を最近傍補間で拡大して取り込めます。音声は OBS の「macOS 音声キャプチャ」（アプリケーション音声）で ReplayNES を選んでください。")
                .font(.caption).foregroundStyle(.secondary)
        }
    }
}

/// Small badge for the viewport overlay.
struct StreamOutputBadge: View {
    @ObservedObject var stream = StreamOutputModel.shared
    var body: some View {
        if stream.active {
            Text("Syphon出力中").font(.system(size: 11, weight: .semibold)).foregroundStyle(.white)
                .padding(.horizontal, 6).padding(.vertical, 2)
                .background(Color.indigo.opacity(0.6), in: RoundedRectangle(cornerRadius: 5))
        }
    }
}
