// Offline MP4 export: drives an rn_renderer (fresh core, logical time) and encodes with
// AVAssetWriter. Timestamps come only from frame / sample counts, never from the wall clock.
// UI-free so it can be unit tested headlessly.
// SPDX-License-Identifier: GPL-2.0-or-later
import AVFoundation
import CoreMedia
import CoreVideo
import Foundation
import Metal

struct ExportSettings: Equatable {
    enum Codec: String, CaseIterable, Identifiable {
        case h264, hevc
        var id: String { rawValue }
        var label: String { self == .h264 ? "H.264" : "HEVC (H.265)" }
        var avCodec: AVVideoCodecType { self == .h264 ? .h264 : .hevc }
    }

    enum SizePreset: Hashable, Identifiable {
        case native(Int)         // cropped source x N (pixel aspect applied horizontally)
        case canvas(Int, Int)    // fixed canvas, integer vertical scale, centered, black borders
        var id: String { label }
        var label: String {
            switch self {
            case .native(let n): return "原寸 ×\(n)"
            case .canvas(let w, let h): return "\(w)×\(h)"
            }
        }
        static let all: [SizePreset] = [.native(1), .native(2), .native(3), .native(4), .canvas(1280, 960), .canvas(1920, 1440), .canvas(1920, 1080)]
    }

    var codec: Codec = .h264
    var preset: SizePreset = .canvas(1280, 960)
    var cropTop = 8, cropBottom = 8, cropLeft = 0, cropRight = 0
    var pixelAspect87 = false
    var startFrame: UInt64 = 0
    var endFrame: UInt64 = 0          // 0 = take end
    var audioBitrate = 192_000
    var videoBitsPerPixel = 0.25      // per frame; pixel art needs more than camera footage
    /// Photosensitive flash reduction applied to the exported picture only (the renderer, its
    /// hash and the project are unaffected). .off = the exact emulated frames.
    var flashReduction: FlashLevel = .off
    /// 「ブラウン管効果を適用」: the physical CRT model (same Metal pipeline as the live view),
    /// rendered offline frame by frame in order (deterministic for the frame sequence). nil = off.
    var crt: CRTRenderer.Settings?

    func validate() throws {
        let ok = cropTop >= 0 && cropBottom >= 0 && cropLeft >= 0 && cropRight >= 0
            && cropTop + cropBottom <= Int(RN_VIDEO_HEIGHT) - 16 && cropLeft + cropRight <= Int(RN_VIDEO_WIDTH) - 16
        if !ok { throw ExportError.invalidSettings("オーバースキャンのクロップ量が大きすぎます") }
        if endFrame != 0 && endFrame <= startFrame { throw ExportError.invalidSettings("書き出し範囲が空です") }
    }
}

/// Nearest-neighbour mapping from the cropped 256x240 source into the output canvas.
struct ExportGeometry: Equatable {
    var canvasWidth: Int, canvasHeight: Int
    var dstX: Int, dstY: Int, dstWidth: Int, dstHeight: Int
    var srcX: Int, srcY: Int, srcWidth: Int, srcHeight: Int
    var verticalScale: Int

    init(_ s: ExportSettings) {
        let sw = Int(RN_VIDEO_WIDTH) - s.cropLeft - s.cropRight
        let sh = Int(RN_VIDEO_HEIGHT) - s.cropTop - s.cropBottom
        let par = s.pixelAspect87 ? 8.0 / 7.0 : 1.0
        func even(_ v: Int) -> Int { v + (v & 1) }
        func width(_ scale: Int) -> Int { Int((Double(sw * scale) * par).rounded()) }
        var k: Int, dw: Int, dh: Int, cw: Int, ch: Int
        switch s.preset {
        case .native(let n):
            k = max(1, n)
            dw = even(width(k)); dh = sh * k
            cw = dw; ch = even(dh)
        case .canvas(let w, let h):
            k = max(1, h / sh)
            while k > 1 && width(k) > w { k -= 1 }
            dh = min(h, sh * k); dw = min(w, width(k))
            cw = even(w); ch = even(h)
        }
        srcX = s.cropLeft; srcY = s.cropTop; srcWidth = sw; srcHeight = sh
        verticalScale = k
        canvasWidth = cw; canvasHeight = ch; dstWidth = dw; dstHeight = dh
        dstX = (cw - dw) / 2
        dstY = (ch - dh) / 2
    }

    /// Source column for every destination column (exact integer repeat for 1:1).
    func columnMap() -> [Int] { (0..<dstWidth).map { srcX + ($0 * srcWidth) / dstWidth } }
    func rowMap() -> [Int] { (0..<dstHeight).map { srcY + ($0 * srcHeight) / dstHeight } }
}

enum ExportError: Error, LocalizedError {
    case invalidSettings(String)
    case writer(String)
    case cancelled
    case engine(RNError)
    var errorDescription: String? {
        switch self {
        case .invalidSettings(let m): return m
        case .writer(let m): return "エンコードに失敗しました: \(m)"
        case .cancelled: return "書き出しをキャンセルしました"
        case .engine(let e): return e.errorDescription
        }
    }
}

struct ExportResult {
    let frames: UInt64
    let audioSamples: UInt64
    let rendererHash: UInt64
    let duration: Double
    let url: URL
}

/// Encodes one rn_renderer to an MP4 file. Takes ownership of the renderer (frees it).
final class MP4Exporter {
    private let renderer: OpaquePointer
    private let settings: ExportSettings
    private let url: URL
    let geometry: ExportGeometry

    init(renderer: OpaquePointer, settings: ExportSettings, url: URL) {
        self.renderer = renderer
        self.settings = settings
        self.url = url
        self.geometry = ExportGeometry(settings)
    }
    deinit { rn_renderer_free(renderer) }

    var totalFrames: UInt64 { rn_renderer_total_frames(renderer) }

    /// Blocking; run on a background thread. progress(done, total) is called from this thread.
    func run(progress: @escaping (UInt64, UInt64) -> Void, isCancelled: @escaping () -> Bool) throws -> ExportResult {
        try settings.validate()
        let total = rn_renderer_total_frames(renderer)
        if total == 0 { throw ExportError.invalidSettings("書き出すフレームがありません (テイクが空です)") }

        try? FileManager.default.removeItem(at: url)
        let writer: AVAssetWriter
        do { writer = try AVAssetWriter(outputURL: url, fileType: .mp4) } catch { throw ExportError.writer(error.localizedDescription) }
        writer.movieTimeScale = CMTimeScale(RN_FPS_NUM)

        let g = geometry
        let pixels = Double(g.canvasWidth * g.canvasHeight)
        let bitrate = max(2_000_000, Int(pixels * 60.0 * settings.videoBitsPerPixel))
        var compression: [String: Any] = [
            AVVideoAverageBitRateKey: bitrate,
            AVVideoExpectedSourceFrameRateKey: 60,
            AVVideoMaxKeyFrameIntervalKey: 120,
        ]
        if settings.codec == .h264 { compression[AVVideoProfileLevelKey] = AVVideoProfileLevelH264HighAutoLevel }
        let videoSettings: [String: Any] = [
            AVVideoCodecKey: settings.codec.avCodec,
            AVVideoWidthKey: g.canvasWidth,
            AVVideoHeightKey: g.canvasHeight,
            AVVideoCompressionPropertiesKey: compression,
            AVVideoColorPropertiesKey: [
                AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
            ],
        ]
        let vIn = AVAssetWriterInput(mediaType: .video, outputSettings: videoSettings)
        vIn.expectsMediaDataInRealTime = false
        vIn.mediaTimeScale = CMTimeScale(RN_FPS_NUM)
        let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: vIn, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: g.canvasWidth,
            kCVPixelBufferHeightKey as String: g.canvasHeight,
        ])
        let aIn = AVAssetWriterInput(mediaType: .audio, outputSettings: [
            AVFormatIDKey: kAudioFormatMPEG4AAC,
            AVSampleRateKey: Int(RN_SAMPLE_RATE),
            AVNumberOfChannelsKey: 1,
            AVEncoderBitRateKey: settings.audioBitrate,
        ])
        aIn.expectsMediaDataInRealTime = false
        guard writer.canAdd(vIn), writer.canAdd(aIn) else { throw ExportError.writer("入力を追加できません") }
        writer.add(vIn)
        writer.add(aIn)
        guard writer.startWriting() else { throw ExportError.writer(writer.error?.localizedDescription ?? "startWriting") }
        writer.startSession(atSourceTime: .zero)

        var asbd = AudioStreamBasicDescription(
            mSampleRate: Double(RN_SAMPLE_RATE), mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
            mBytesPerPacket: 2, mFramesPerPacket: 1, mBytesPerFrame: 2, mChannelsPerFrame: 1,
            mBitsPerChannel: 16, mReserved: 0)
        var audioFormat: CMAudioFormatDescription?
        guard CMAudioFormatDescriptionCreate(allocator: nil, asbd: &asbd, layoutSize: 0, layout: nil,
                                             magicCookieSize: 0, magicCookie: nil, extensions: nil,
                                             formatDescriptionOut: &audioFormat) == noErr, let audioFormat
        else { throw ExportError.writer("audio format") }

        let cols = g.columnMap(), rows = g.rowMap()
        // Frames are rendered strictly in order, so one filter instance sees a continuous sequence.
        let flash = settings.flashReduction == .off ? nil : FlashFilter(level: settings.flashReduction)
        var filtered = [UInt32](repeating: 0, count: Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT))
        let startFrame = settings.startFrame
        let sampleBase = rn_audio_samples_before(startFrame)
        let frameBytes = Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT)

        // Both inputs pull through requestMediaDataWhenReady (AVFoundation decides the
        // interleaving). Rendering happens on demand when either queue runs dry; pending video
        // is kept as raw 256x240 frames so memory stays small even if one input runs ahead.
        let lock = NSLock()
        var videoQueue: [(pixels: [UInt32], pts: CMTime, signal: CRTExportFrame?)] = []
        let crt = try settings.crt.map { try CRTExportRenderer(settings: $0, geometry: g, crop: settings) }
        var audioQueue: [CMSampleBuffer] = []
        var sourceDone = false
        var failure: Error?
        var framesWritten: UInt64 = 0
        var samplesWritten: UInt64 = 0

        /// Must hold `lock`. Renders the next frame into both queues.
        func produce() {
            if sourceDone || failure != nil { return }
            if isCancelled() { failure = ExportError.cancelled; return }
            var video: UnsafePointer<UInt32>?
            var audio: UnsafePointer<Int16>?
            var n = 0
            var f: UInt64 = 0
            let st = rn_renderer_next(renderer, &video, &audio, &n, &f)
            if st == RN_ERR_END_OF_TAKE { sourceDone = true; return }
            if st != RN_OK { failure = ExportError.engine(RNError(st)); return }
            guard let video else { failure = ExportError.writer("no video"); return }
            // Video PTS = frame offset * 655171 / 39375000 s (exact rational).
            let pts = CMTime(value: CMTimeValue((f - startFrame) * UInt64(RN_FPS_DEN)), timescale: CMTimeScale(RN_FPS_NUM))
            // CRT signal side channel of the same picture (display only).
            var signal: CRTExportFrame?
            if crt != nil {
                var info = rn_video_indices_info()
                signal = CRTExportFrame(codes: rn_renderer_video_indices(renderer, &info) == RN_OK && info.codes != nil
                                            ? Array(UnsafeBufferPointer(start: info.codes, count: frameBytes)) : nil,
                                        burstPhase: info.burst_phase, ordinal: info.codes != nil ? info.frame : f, flashAltered: false)
            }
            if let flash {
                let altered = filtered.withUnsafeMutableBufferPointer { flash.process(video, into: $0.baseAddress!) }
                signal?.flashAltered = altered
                videoQueue.append((filtered, pts, signal))
            } else {
                videoQueue.append((Array(UnsafeBufferPointer(start: video, count: frameBytes)), pts, signal))
            }
            if n > 0, let audio {
                // Audio PTS from the absolute sample count at 48 kHz.
                let apts = CMTime(value: CMTimeValue(rn_audio_samples_before(f) - sampleBase), timescale: CMTimeScale(RN_SAMPLE_RATE))
                do {
                    audioQueue.append(try MP4Exporter.audioSampleBuffer(audio, count: n, pts: apts, format: audioFormat))
                    samplesWritten += UInt64(n)
                } catch { failure = error; return }
            }
            progress(rn_renderer_frames_done(renderer), total)
        }

        let group = DispatchGroup()
        group.enter()
        group.enter()
        let vQueue = DispatchQueue(label: "replaynes.export.video")
        let aQueue = DispatchQueue(label: "replaynes.export.audio")
        var vFinished = false, aFinished = false

        vIn.requestMediaDataWhenReady(on: vQueue) {
            if vFinished { return }
            while vIn.isReadyForMoreMediaData {
                lock.lock()
                if videoQueue.isEmpty { produce() }
                if failure != nil || (videoQueue.isEmpty && sourceDone) {
                    lock.unlock()
                    vFinished = true
                    vIn.markAsFinished()
                    group.leave()
                    return
                }
                let item = videoQueue.removeFirst()
                lock.unlock()
                guard let pool = adaptor.pixelBufferPool else { continue }
                var pbOut: CVPixelBuffer?
                guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pbOut) == kCVReturnSuccess, let pb = pbOut else {
                    lock.lock(); failure = failure ?? ExportError.writer("pixel buffer"); lock.unlock()
                    continue
                }
                if let crt, let signal = item.signal {
                    if !crt.render(pixels: item.pixels, signal: signal, into: pb) {
                        lock.lock(); failure = failure ?? ExportError.writer("CRT rendering failed"); lock.unlock()
                        continue
                    }
                } else {
                    item.pixels.withUnsafeBufferPointer { MP4Exporter.scale($0.baseAddress!, into: pb, geometry: g, cols: cols, rows: rows) }
                }
                if !adaptor.append(pb, withPresentationTime: item.pts) {
                    lock.lock(); failure = failure ?? ExportError.writer(writer.error?.localizedDescription ?? "append video"); lock.unlock()
                    continue
                }
                lock.lock(); framesWritten += 1; lock.unlock()
            }
        }
        aIn.requestMediaDataWhenReady(on: aQueue) {
            if aFinished { return }
            while aIn.isReadyForMoreMediaData {
                lock.lock()
                if audioQueue.isEmpty { produce() }
                if failure != nil || (audioQueue.isEmpty && sourceDone) {
                    lock.unlock()
                    aFinished = true
                    aIn.markAsFinished()
                    group.leave()
                    return
                }
                if audioQueue.isEmpty { lock.unlock(); continue } // frame without audio
                let sb = audioQueue.removeFirst()
                lock.unlock()
                if !aIn.append(sb) {
                    lock.lock(); failure = failure ?? ExportError.writer(writer.error?.localizedDescription ?? "append audio"); lock.unlock()
                }
            }
        }
        group.wait()

        if let failure {
            writer.cancelWriting()
            try? FileManager.default.removeItem(at: url)
            throw failure
        }
        let endTime = CMTime(value: CMTimeValue(framesWritten * UInt64(RN_FPS_DEN)), timescale: CMTimeScale(RN_FPS_NUM))
        writer.endSession(atSourceTime: endTime)
        let done = DispatchSemaphore(value: 0)
        writer.finishWriting { done.signal() }
        done.wait()
        if writer.status != .completed {
            try? FileManager.default.removeItem(at: url)
            throw ExportError.writer(writer.error?.localizedDescription ?? "finishWriting status \(writer.status.rawValue)")
        }
        return ExportResult(frames: framesWritten, audioSamples: samplesWritten,
                            rendererHash: rn_renderer_hash(renderer), duration: endTime.seconds, url: url)
    }

    /// Nearest-neighbour scale BGRA 256x240 -> canvas (black borders). Rows are built once and
    /// memcpy'd for vertical repeats.
    static func scale(_ src: UnsafePointer<UInt32>, into pb: CVPixelBuffer, geometry g: ExportGeometry, cols: [Int], rows: [Int]) {
        CVPixelBufferLockBaseAddress(pb, [])
        defer { CVPixelBufferUnlockBaseAddress(pb, []) }
        guard let base = CVPixelBufferGetBaseAddress(pb) else { return }
        let bpr = CVPixelBufferGetBytesPerRow(pb)
        let w = Int(RN_VIDEO_WIDTH)
        let black: UInt32 = 0xFF00_0000 // BGRA little-endian: A=255
        cols.withUnsafeBufferPointer { cmap in
            var lastSrcRow = -1
            var lastDstRow: UnsafeMutablePointer<UInt32>?
            for y in 0..<g.canvasHeight {
                let row = (base + y * bpr).assumingMemoryBound(to: UInt32.self)
                let dy = y - g.dstY
                if dy < 0 || dy >= g.dstHeight {
                    row.update(repeating: black, count: g.canvasWidth)
                    continue
                }
                let sy = rows[dy]
                if sy == lastSrcRow, let prev = lastDstRow {
                    row.update(from: prev, count: g.canvasWidth)
                    continue
                }
                if g.dstX > 0 { row.update(repeating: black, count: g.dstX) }
                let right = g.dstX + g.dstWidth
                if right < g.canvasWidth { (row + right).update(repeating: black, count: g.canvasWidth - right) }
                let srow = src + sy * w
                let out = row + g.dstX
                for x in 0..<g.dstWidth { out[x] = srow[cmap[x]] | black }
                lastSrcRow = sy
                lastDstRow = row
            }
        }
    }

    static func audioSampleBuffer(_ pcm: UnsafePointer<Int16>, count n: Int, pts: CMTime, format: CMAudioFormatDescription) throws -> CMSampleBuffer {
        var block: CMBlockBuffer?
        let bytes = n * 2
        guard CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: nil, blockLength: bytes, blockAllocator: nil,
                                                 customBlockSource: nil, offsetToData: 0, dataLength: bytes,
                                                 flags: kCMBlockBufferAssureMemoryNowFlag, blockBufferOut: &block) == noErr,
              let block,
              CMBlockBufferReplaceDataBytes(with: pcm, blockBuffer: block, offsetIntoDestination: 0, dataLength: bytes) == noErr
        else { throw ExportError.writer("audio block buffer") }
        var sb: CMSampleBuffer?
        guard CMAudioSampleBufferCreateReadyWithPacketDescriptions(allocator: nil, dataBuffer: block, formatDescription: format,
                                                                   sampleCount: n, presentationTimeStamp: pts,
                                                                   packetDescriptions: nil, sampleBufferOut: &sb) == noErr,
              let sb
        else { throw ExportError.writer("audio sample buffer") }
        return sb
    }
}

/// One exported frame's CRT input (raw PPU codes when the core has them).
struct CRTExportFrame {
    var codes: [UInt16]?
    var burstPhase: UInt32
    var ordinal: UInt64
    var flashAltered: Bool
}

/// Offline CRT rendering for the exporter: the live pipeline (CRTRenderer), synchronous plan,
/// one frame at a time in order, read back into the encoder's pixel buffer.
final class CRTExportRenderer {
    private let renderer: CRTRenderer
    private let queue: MTLCommandQueue
    private let target: MTLTexture
    private let dst: CGRect
    private let cropFraction: Double

    init(settings: CRTRenderer.Settings, geometry g: ExportGeometry, crop: ExportSettings) throws {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
            throw ExportError.writer("Metal を利用できません（ブラウン管効果）")
        }
        renderer = try CRTRenderer(device: device, targetPixelFormat: nil)
        self.queue = queue
        cropFraction = Double(crop.cropTop + crop.cropBottom) / 2 / 240
        // Full 4:3 raster minus the cropped overscan rows, fitted into the canvas and centred.
        let aspect = (4.0 / 3.0) / (1 - 2 * cropFraction)
        var h = Double(g.canvasHeight), w = (h * aspect).rounded()
        if w > Double(g.canvasWidth) { w = Double(g.canvasWidth); h = (w / aspect).rounded() }
        dst = CGRect(x: ((Double(g.canvasWidth) - w) / 2).rounded(), y: ((Double(g.canvasHeight) - h) / 2).rounded(), width: w, height: h)
        let (tw, th) = CRTRenderer.tubeSize(forDestination: dst.size, cropFraction: cropFraction, maxWidth: 1600)
        renderer.configure(settings: settings, outputWidth: tw, outputHeight: th, synchronous: true)
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: g.canvasWidth, height: g.canvasHeight, mipmapped: false)
        td.usage = [.shaderWrite, .shaderRead]
        td.storageMode = .shared
        guard let t = device.makeTexture(descriptor: td) else { throw ExportError.writer("CRT texture") }
        target = t
    }

    var destination: CGRect { dst }

    /// Same input rule as the live view: RF path from the PPU codes, or the (flash-filtered) RGB
    /// picture when the filter altered the frame / the core has no codes.
    func render(pixels: [UInt32], signal: CRTExportFrame, into pb: CVPixelBuffer) -> Bool {
        guard let cb = queue.makeCommandBuffer() else { return false }
        let ok: Bool
        if let codes = signal.codes, !signal.flashAltered {
            ok = codes.withUnsafeBufferPointer { renderer.encode(.codes($0.baseAddress!, burstPhase: signal.burstPhase), ordinal: signal.ordinal, into: cb) }
        } else {
            ok = pixels.withUnsafeBufferPointer { renderer.encode(.rgb($0.baseAddress!), ordinal: signal.ordinal, into: cb) }
        }
        guard ok else { return false }
        renderer.encodeShow(into: target, cb: cb, dst: dst, cropFraction: cropFraction)
        cb.commit()
        cb.waitUntilCompleted()
        if cb.error != nil { return false }
        CVPixelBufferLockBaseAddress(pb, [])
        defer { CVPixelBufferUnlockBaseAddress(pb, []) }
        guard let base = CVPixelBufferGetBaseAddress(pb) else { return false }
        let bpr = CVPixelBufferGetBytesPerRow(pb)
        target.getBytes(base, bytesPerRow: bpr, from: MTLRegionMake2D(0, 0, target.width, target.height), mipmapLevel: 0)
        return true
    }
}
