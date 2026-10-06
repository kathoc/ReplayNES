// Filmstrip thumbnails for the timeline: grid of sampled frames, the thread-safe cache and the
// 256x240 -> 128x120 downscaler. The grid math, the cache policy and the box filter live in the
// shared frontend core (frontend/src/timeline.cpp, thumbcache.cpp); this file adds CGImage.
// Thumbnail keys are CURSOR frames f (> 0): the picture shown at rn_frame() == f.
// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import Foundation

enum ThumbnailGrid {
    // The timeline has a fixed scale: one tile = F frames of game time = one picture at its
    // natural width (tileWidth), F = 5 s * 2^k, the smallest step at which the take fits.

    static let baseStep: Double = RNF_THUMB_BASE_STEP
    static let minExponent = Int(RNF_THUMB_MIN_EXPONENT)
    static let maxExponent = Int(RNF_THUMB_MAX_EXPONENT)

    static func step(exponent e: Int) -> Double { rnf_thumb_step(Int32(clamping: e)) }
    static var minStep: Double { step(exponent: minExponent) }

    /// True for baseStep * 2^e (minExponent...maxExponent).
    static func isStep(_ f: Double) -> Bool { rnf_thumb_is_step(f) != 0 }

    /// Cursor frame of grid point k.
    static func frame(_ k: UInt64, step f: Double) -> UInt64 { rnf_thumb_frame(k, f) }

    private static func list(_ fn: (UnsafeMutablePointer<UInt64>?, Int) -> Int) -> [UInt64] {
        let n = fn(nil, 0)
        guard n > 0 else { return [] }
        var out = [UInt64](repeating: 0, count: n)
        _ = out.withUnsafeMutableBufferPointer { fn($0.baseAddress, n) }
        return out
    }

    /// Grid frames F, 2F, ... <= length.
    static func frames(length: UInt64, step f: Double) -> [UInt64] { list { rnf_thumb_frames(length, f, $0, $1) } }

    static func isGridFrame(_ x: UInt64, step f: Double) -> Bool { rnf_thumb_is_grid_frame(x, f) != 0 }

    /// The screen 1 s in: the first frames after power-on are blank.
    static let startFrame = UInt64(RNF_THUMB_START_FRAME)

    /// Picture of tile 0 (anchored at frame 0): 1 s in (never a later tile's picture).
    static func startPicture(step f: Double) -> UInt64 { rnf_thumb_start_picture(f) }

    static func pictureFrame(tile k: UInt64, step f: Double) -> UInt64 { rnf_thumb_picture_frame(k, f) }

    /// Pictures needed for a take of `length` frames at step F (sorted).
    static func targets(length: UInt64, step f: Double) -> [UInt64] { list { rnf_thumb_targets(length, f, $0, $1) } }

    /// Width of the recorded part of the bar: length frames at tileWidth per F frames.
    static func extent(length: UInt64, step f: Double, tileWidth: Double) -> Double { rnf_thumb_extent(length, f, tileWidth) }

    /// Frames per tile: the smallest 5 s * 2^k at which the whole take fits `width`.
    static func tileStep(width: Double, tileWidth: Double, length: UInt64) -> Double {
        rnf_thumb_tile_step(width, tileWidth, length)
    }

    /// One tile: anchored at `frame` (grid point `index`), showing `picture` at its natural size
    /// from `x`; `visible` is the part left of the recorded extent (tileWidth for full tiles).
    struct Tile: Equatable {
        var index: UInt64
        var frame: UInt64
        var picture: UInt64
        var x: Double
        var visible: Double
    }

    /// Tiles whose interval has started (k*F < length).
    static func tiles(length: UInt64, step f: Double, tileWidth tw: Double) -> [Tile] {
        let n = rnf_thumb_tiles(length, f, tw, nil, 0)
        guard n > 0 else { return [] }
        var out = [rnf_thumb_tile](repeating: rnf_thumb_tile(), count: n)
        _ = rnf_thumb_tiles(length, f, tw, &out, n)
        return out.map { Tile(index: $0.index, frame: $0.frame, picture: $0.picture, x: $0.x, visible: $0.visible) }
    }

    /// x snapped down to a whole backing pixel (the playhead moves in clean 1-dot steps).
    static func snapToPixel(_ x: Double, scale: Double) -> Double { rnf_thumb_snap_to_pixel(x, scale) }

    /// While a tile's own picture is being made it shows the latest EARLIER picture within one
    /// step, dimmed.
    static func fallbackWindow(step f: Double) -> UInt64 { rnf_thumb_fallback_window(f) }
    static let fallbackOpacity = RNF_THUMB_FALLBACK_OPACITY
}

enum ThumbnailScaler {
    static let factor = Int(RNF_THUMB_FACTOR)
    static let width = Int(RNF_THUMB_WIDTH)
    static let height = Int(RNF_THUMB_HEIGHT)

    /// 256x240 BGRA8 (rn_video) -> 128x120 by 2x2 box averaging.
    static func downscale(_ src: UnsafePointer<UInt32>) -> [UInt32] {
        var out = [UInt32](repeating: 0, count: width * height)
        out.withUnsafeMutableBufferPointer { rnf_thumb_downscale(src, $0.baseAddress) }
        return out
    }

    static func image(_ px: [UInt32]) -> CGImage? {
        guard px.count == width * height else { return nil }
        let data = px.withUnsafeBufferPointer { Data(buffer: $0) } as CFData
        guard let provider = CGDataProvider(data: data) else { return nil }
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                       space: CGColorSpaceCreateDeviceRGB(),
                       bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue
                                                | CGImageAlphaInfo.noneSkipFirst.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }

    static func thumbnail(_ src: UnsafePointer<UInt32>) -> CGImage? { image(downscale(src)) }
}

/// Thumbnails of the active take keyed by cursor frame (rnf_thumb_cache holding CGImages).
/// Thread-safe: written by the emulation thread and the generator thread, read by the UI.
final class ThumbnailCache {
    private let handle: OpaquePointer

    /// Called after every change, on the thread that made it (keep it cheap).
    var onChange: (() -> Void)?

    init() {
        handle = rnf_thumb_cache_new({ _ = Unmanaged<CGImage>.fromOpaque($0!).retain() },
                                     { Unmanaged<CGImage>.fromOpaque($0!).release() })!
        rnf_thumb_cache_set_on_change(handle, { ctx in
            Unmanaged<ThumbnailCache>.fromOpaque(ctx!).takeUnretainedValue().onChange?()
        }, Unmanaged.passUnretained(self).toOpaque())
    }

    deinit { rnf_thumb_cache_free(handle) }

    /// Upper bound on kept images; extra images off the current grid are evicted.
    var capacity: Int {
        get { rnf_thumb_cache_capacity(handle) }
        set { rnf_thumb_cache_set_capacity(handle, max(0, newValue)) }
    }

    var takeID: UInt64 { rnf_thumb_cache_take(handle) }
    /// Bumped on every session reset and take change: jobs started earlier are stale.
    var generation: UInt64 { rnf_thumb_cache_generation(handle) }
    var version: UInt64 { rnf_thumb_cache_version(handle) }
    var count: Int { rnf_thumb_cache_count(handle) }
    var gridStep: Double { rnf_thumb_cache_step(handle) }

    /// New session (or none): forget everything.
    func reset(take t: UInt64) { rnf_thumb_cache_reset(handle, t) }

    /// The active take changed from takeID to `t`; frames <= keepThrough stay valid.
    func rebase(toTake t: UInt64, keepThrough p: UInt64) { rnf_thumb_cache_rebase(handle, t, p) }

    /// Frames per tile the UI currently wants (0 = none). Live capture only stores its pictures.
    func setStep(_ q: Double) { rnf_thumb_cache_set_step(handle, q) }

    /// Cheap check for the emulation thread: is cursor frame f a wanted picture not cached yet?
    func wants(frame f: UInt64, take t: UInt64) -> Bool { rnf_thumb_cache_wants(handle, f, t) != 0 }

    /// Stores an image made on take t (generation g, if given). Rejected if the take changed since.
    @discardableResult
    func insert(frame f: UInt64, take t: UInt64, generation g: UInt64? = nil, image: CGImage) -> Bool {
        withExtendedLifetime(image) {
            rnf_thumb_cache_insert(handle, f, t, g == nil ? 0 : 1, g ?? 0, Unmanaged.passUnretained(image).toOpaque()) != 0
        }
    }

    /// Stores a batch (one change notification). Rejected if the take or generation changed.
    @discardableResult
    func insert(batch: [(UInt64, CGImage)], take t: UInt64, generation g: UInt64) -> Bool {
        withExtendedLifetime(batch) {
            let frames = batch.map(\.0)
            let payloads: [UnsafeMutableRawPointer?] = batch.map { Unmanaged.passUnretained($0.1).toOpaque() }
            return rnf_thumb_cache_insert_batch(handle, frames, payloads, batch.count, t, g) != 0
        }
    }

    func contains(_ f: UInt64) -> Bool { rnf_thumb_cache_contains(handle, f) != 0 }

    func missing(_ frames: [UInt64]) -> [UInt64] {
        var out = [UInt64](repeating: 0, count: frames.count)
        let n = rnf_thumb_cache_missing(handle, frames, frames.count, &out, out.count)
        return Array(out.prefix(n))
    }

    private func take(_ p: UnsafeMutableRawPointer?) -> CGImage? {
        guard let p else { return nil }
        return Unmanaged<CGImage>.fromOpaque(p).takeRetainedValue()
    }

    /// Image at exactly frame f.
    func image(at f: UInt64) -> CGImage? { take(rnf_thumb_cache_image_at(handle, f)) }

    /// Latest cached image at a frame in [f - window, f).
    func image(before f: UInt64, within window: UInt64) -> CGImage? { take(rnf_thumb_cache_image_before(handle, f, window)) }
}
