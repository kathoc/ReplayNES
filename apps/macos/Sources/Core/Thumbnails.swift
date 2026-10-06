// Filmstrip thumbnails for the timeline: grid of sampled frames, the thread-safe cache and the
// 256x240 -> 128x120 downscaler. Never used on the emulation hot path except for an O(1) "is this
// frame wanted?" check after a step (live capture of frames that are displayed anyway).
// Thumbnail keys are CURSOR frames f (> 0): the picture shown at rn_frame() == f, i.e. the output
// of input record f-1. It depends only on records [0, f), so it is valid on every take that
// shares that prefix (TakeLineage.sharedPrefix).
// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import Foundation

enum ThumbnailGrid {
    // MARK: filmstrip layout (video-editor style, fixed scale)
    //
    // The timeline has a fixed scale: one tile = F frames of game time = one picture at its
    // natural width (tileWidth), F = 5 s * 2^k. The recorded part of the take occupies
    // x = 0 ... length * tileWidth / F (the extent) and grows to the right while recording; the
    // strip to the right of it stays empty. Tile k starts at x = k * tileWidth, shows the picture
    // at frame k*F (tile 0: startPicture) at its natural size and position, clipped at the extent:
    // the newest tile appears clipped as soon as its interval starts and is revealed as the take
    // grows (fully shown after F frames). F is the smallest step at which the whole take fits the
    // strip: when the extent would pass the strip end, F doubles (the bar halves, crossfaded).

    /// 5 s of game time. Every step is baseStep * 2^e: a coarser grid is a subset of every finer
    /// one, so a growing take or a narrower window reuses the pictures already made.
    static let baseStep: Double = 300
    static let minExponent = 0    // one picture per 5 s at most
    static let maxExponent = 40

    static func step(exponent e: Int) -> Double { scalbn(baseStep, max(minExponent, min(maxExponent, e))) }
    static var minStep: Double { step(exponent: minExponent) }

    /// True for baseStep * 2^e (minExponent...maxExponent).
    static func isStep(_ f: Double) -> Bool {
        guard f > 0, f.isFinite else { return false }
        let e = Int((log2(f / baseStep)).rounded())
        return e >= minExponent && e <= maxExponent && step(exponent: e) == f
    }

    /// Cursor frame of grid point k (k * F rounded; exact for coarser grids, which are subsets).
    static func frame(_ k: UInt64, step f: Double) -> UInt64 { UInt64((Double(k) * f).rounded()) }

    /// Grid frames F, 2F, ... <= length.
    static func frames(length: UInt64, step f: Double) -> [UInt64] {
        guard f > 0, Double(length) >= f else { return [] }
        return (1...UInt64((Double(length) / f).rounded(.down))).map { frame($0, step: f) }
    }

    static func isGridFrame(_ x: UInt64, step f: Double) -> Bool {
        guard x > 0, f > 0 else { return false }
        return frame(UInt64((Double(x) / f).rounded()), step: f) == x
    }

    /// The screen 1 s in: the first frames after power-on are blank (flat gray / green / black on
    /// every game).
    static let startFrame: UInt64 = 60

    /// Picture of tile 0 (anchored at frame 0, where nothing has been drawn yet): 1 s in (never a
    /// later tile's picture).
    static func startPicture(step f: Double) -> UInt64 { min(startFrame, max(1, UInt64((f / 2).rounded()))) }

    static func pictureFrame(tile k: UInt64, step f: Double) -> UInt64 { k == 0 ? startPicture(step: f) : frame(k, step: f) }

    /// Pictures needed for a take of `length` frames at step F (sorted).
    static func targets(length: UInt64, step f: Double) -> [UInt64] {
        guard length > 0, f > 0 else { return [] }
        let last = UInt64((Double(length) / f).rounded(.down))
        var out: [UInt64] = []
        var k: UInt64 = 0
        while k <= last {
            let p = pictureFrame(tile: k, step: f)
            if p <= length, p > (out.last ?? 0) { out.append(p) }
            k += 1
        }
        return out
    }

    /// Width of the recorded part of the bar: length frames at tileWidth per F frames.
    static func extent(length: UInt64, step f: Double, tileWidth: Double) -> Double {
        guard f > 0, tileWidth > 0 else { return 0 }
        return Double(length) / f * tileWidth
    }

    /// Frames per tile: the smallest 5 s * 2^k at which the whole take fits `width`.
    static func tileStep(width: Double, tileWidth: Double, length: UInt64) -> Double {
        var f = minStep
        guard width > 0, tileWidth > 0 else { return f }
        while f < step(exponent: maxExponent) && extent(length: length, step: f, tileWidth: tileWidth) > width + 1e-9 { f *= 2 }
        return f
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

    /// Tiles whose interval has started (k*F < length): ceil(length / F) of them; all have the
    /// natural width, only the newest one is clipped at the extent.
    static func tiles(length: UInt64, step f: Double, tileWidth tw: Double) -> [Tile] {
        guard length > 0, f > 0, tw > 0 else { return [] }
        let end = extent(length: length, step: f, tileWidth: tw)
        var out: [Tile] = []
        var k: UInt64 = 0
        while Double(k) * f < Double(length) {
            let x = Double(k) * tw
            out.append(Tile(index: k, frame: frame(k, step: f), picture: pictureFrame(tile: k, step: f), x: x,
                            visible: max(0, min(tw, end - x))))
            k += 1
        }
        return out
    }

    /// x snapped down to a whole backing pixel (the playhead moves in clean 1-dot steps).
    static func snapToPixel(_ x: Double, scale: Double) -> Double {
        guard scale > 0 else { return x }
        return (x * scale + 1e-9).rounded(.down) / scale
    }

    /// While a tile's own picture is being made (e.g. right after the step halved: window made
    /// wider), it shows the latest EARLIER picture within one step, dimmed. Farther or later
    /// pictures would be misdated: the tile then stays empty until its own picture exists.
    static func fallbackWindow(step f: Double) -> UInt64 { UInt64(f.rounded(.up)) }
    static let fallbackOpacity = 0.45
}

enum ThumbnailScaler {
    /// Half resolution: a tile wider than its natural width shows the picture enlarged
    /// (ThumbnailGrid.fill), so a quarter-size picture would be too coarse.
    static let factor = 2
    static let width = Int(RN_VIDEO_WIDTH) / factor
    static let height = Int(RN_VIDEO_HEIGHT) / factor

    /// 256x240 BGRA8 (rn_video) -> 128x120 by 2x2 box averaging.
    static func downscale(_ src: UnsafePointer<UInt32>) -> [UInt32] {
        let sw = Int(RN_VIDEO_WIDTH), k = factor
        let n = UInt32(k * k)
        var out = [UInt32](repeating: 0xFF00_0000, count: width * height)
        for y in 0..<height {
            for x in 0..<width {
                var r: UInt32 = 0, g: UInt32 = 0, b: UInt32 = 0
                for dy in 0..<k {
                    let row = src + (y * k + dy) * sw + x * k
                    for dx in 0..<k {
                        let p = row[dx]
                        b &+= p & 0xFF
                        g &+= (p >> 8) & 0xFF
                        r &+= (p >> 16) & 0xFF
                    }
                }
                out[y * width + x] = 0xFF00_0000 | ((r / n) << 16) | ((g / n) << 8) | (b / n)
            }
        }
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

/// Thumbnails of the active take keyed by cursor frame. Thread-safe: written by the emulation
/// thread (live capture, take/session changes) and the generator thread, read by the UI.
final class ThumbnailCache {
    private let lock = NSLock()
    private var images: [UInt64: CGImage] = [:]
    private var sortedKeys: [UInt64] = []
    private var keysDirty = false
    private var take: UInt64 = 0
    private var generationValue: UInt64 = 1
    private var step: Double = 0
    private var versionValue: UInt64 = 0
    /// Upper bound on kept images; extra images off the current grid are evicted.
    var capacity = 600  // 60 KB each

    /// Called after every change, on the thread that made it (keep it cheap).
    var onChange: (() -> Void)?

    var takeID: UInt64 { lock.lock(); defer { lock.unlock() }; return take }
    /// Bumped on every session reset and take change: jobs started earlier are stale.
    var generation: UInt64 { lock.lock(); defer { lock.unlock() }; return generationValue }
    var version: UInt64 { lock.lock(); defer { lock.unlock() }; return versionValue }
    var count: Int { lock.lock(); defer { lock.unlock() }; return images.count }
    var gridStep: Double { lock.lock(); defer { lock.unlock() }; return step }

    private func changed() {
        versionValue &+= 1
    }

    private func notify() { onChange?() }

    /// New session (or none): forget everything.
    func reset(take t: UInt64) {
        lock.lock()
        images.removeAll()
        sortedKeys.removeAll()
        keysDirty = false
        take = t
        generationValue &+= 1
        changed()
        lock.unlock()
        notify()
    }

    /// The active take changed from takeID to `t`; frames <= keepThrough (the shared prefix of the
    /// two takes) stay valid, later ones are dropped.
    func rebase(toTake t: UInt64, keepThrough p: UInt64) {
        lock.lock()
        if t == take { lock.unlock(); return }
        let before = images.count
        images = images.filter { $0.key <= p }
        if images.count != before { keysDirty = true }
        take = t
        generationValue &+= 1
        changed()
        lock.unlock()
        notify()
    }

    /// Frames per tile the UI currently wants (0 = none). Live capture only stores its pictures.
    func setStep(_ q: Double) {
        lock.lock()
        step = q
        if images.count > capacity, q > 0 {
            images = images.filter { ThumbnailGrid.isGridFrame($0.key, step: q) || $0.key == ThumbnailGrid.startPicture(step: q) }
            keysDirty = true
        }
        lock.unlock()
    }

    /// Cheap check for the emulation thread: is cursor frame f a wanted picture (grid point or
    /// tile 0's picture), on the cached take and not cached yet?
    func wants(frame f: UInt64, take t: UInt64) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard f > 0, step > 0, t == take, images[f] == nil else { return false }
        return ThumbnailGrid.isGridFrame(f, step: step) || f == ThumbnailGrid.startPicture(step: step)
    }

    /// Stores an image made on take t (generation g, if given). Rejected if the take changed since.
    @discardableResult
    func insert(frame f: UInt64, take t: UInt64, generation g: UInt64? = nil, image: CGImage) -> Bool {
        lock.lock()
        guard f > 0, t == take, g == nil || g == generationValue else { lock.unlock(); return false }
        if images.updateValue(image, forKey: f) == nil { keysDirty = true }
        changed()
        lock.unlock()
        notify()
        return true
    }

    /// Stores a batch (one change notification: the filmstrip shows them all at once).
    /// Rejected if the take or generation changed since the batch was started.
    @discardableResult
    func insert(batch: [(UInt64, CGImage)], take t: UInt64, generation g: UInt64) -> Bool {
        lock.lock()
        guard t == take, g == generationValue, !batch.isEmpty else { lock.unlock(); return false }
        for (f, img) in batch where f > 0 {
            if images.updateValue(img, forKey: f) == nil { keysDirty = true }
        }
        changed()
        lock.unlock()
        notify()
        return true
    }

    func contains(_ f: UInt64) -> Bool { lock.lock(); defer { lock.unlock() }; return images[f] != nil }

    func missing(_ frames: [UInt64]) -> [UInt64] {
        lock.lock(); defer { lock.unlock() }
        return frames.filter { images[$0] == nil }
    }

    /// Image at exactly frame f.
    func image(at f: UInt64) -> CGImage? { lock.lock(); defer { lock.unlock() }; return images[f] }

    /// Latest cached image at a frame in [f - window, f).
    func image(before f: UInt64, within window: UInt64) -> CGImage? {
        lock.lock(); defer { lock.unlock() }
        if keysDirty { sortedKeys = images.keys.sorted(); keysDirty = false }
        var lo = 0, hi = sortedKeys.count
        while lo < hi {  // first key >= f
            let mid = (lo + hi) / 2
            if sortedKeys[mid] < f { lo = mid + 1 } else { hi = mid }
        }
        guard lo > 0 else { return nil }
        let k = sortedKeys[lo - 1]
        return f - k <= window ? images[k] : nil
    }
}
