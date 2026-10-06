// Filmstrip thumbnails for the timeline: grid of sampled frames, the thread-safe cache and the
// 256x240 -> 64x60 downscaler. Never used on the emulation hot path except for an O(1) "is this
// frame wanted?" check after a step (live capture of frames that are displayed anyway).
// Thumbnail keys are CURSOR frames f (> 0): the picture shown at rn_frame() == f, i.e. the output
// of input record f-1. It depends only on records [0, f), so it is valid on every take that
// shares that prefix (TakeLineage.sharedPrefix).
// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import Foundation

enum ThumbnailGrid {
    /// Finest spacing (frames).
    static let baseStep: UInt64 = 4

    /// Smallest baseStep * 2^k with at most maxCount grid frames over the take. Power-of-two
    /// steps make coarser grids subsets of finer ones: a growing take or a narrower window reuses
    /// every image already made.
    static func step(length: UInt64, maxCount: Int) -> UInt64 {
        let m = UInt64(max(1, maxCount))
        var q = baseStep
        while length / q > m && q < (UInt64(1) << 40) { q *= 2 }
        return q
    }

    /// Grid frames q, 2q, ... <= length.
    static func frames(length: UInt64, step q: UInt64) -> [UInt64] {
        guard q > 0, length >= q else { return [] }
        return (1...(length / q)).map { $0 * q }
    }

    /// Representative grid frame of each tile (tiles of tileWidth points across width), from the
    /// tile's start position, snapped to the nearest grid frame. Empty if the take is shorter than q.
    static func tileFrames(width: Double, tileWidth: Double, length: UInt64, step q: UInt64) -> [UInt64] {
        guard width > 0, tileWidth > 0, q > 0, length >= q else { return [] }
        let g = TimelineGeometry(width: width, length: length)
        let n = Int((width / tileWidth).rounded(.up))
        let last = length / q * q
        return (0..<n).map { i in
            let f = g.frame(atX: Double(i) * tileWidth)
            let snapped = (f + q / 2) / q * q
            return min(max(snapped, q), last)
        }
    }
}

enum ThumbnailScaler {
    static let width = 64
    static let height = 60

    /// 256x240 BGRA8 (rn_video) -> 64x60 by 4x4 box averaging.
    static func downscale(_ src: UnsafePointer<UInt32>) -> [UInt32] {
        let sw = Int(RN_VIDEO_WIDTH)
        var out = [UInt32](repeating: 0xFF00_0000, count: width * height)
        for y in 0..<height {
            for x in 0..<width {
                var r: UInt32 = 0, g: UInt32 = 0, b: UInt32 = 0
                for dy in 0..<4 {
                    let row = src + (y * 4 + dy) * sw + x * 4
                    for dx in 0..<4 {
                        let p = row[dx]
                        b &+= p & 0xFF
                        g &+= (p >> 8) & 0xFF
                        r &+= (p >> 16) & 0xFF
                    }
                }
                out[y * width + x] = 0xFF00_0000 | ((r / 16) << 16) | ((g / 16) << 8) | (b / 16)
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
    private var step: UInt64 = 0
    private var versionValue: UInt64 = 0
    /// Upper bound on kept images; extra images off the current grid are evicted.
    var capacity = 2_000

    /// Called after every change, on the thread that made it (keep it cheap).
    var onChange: (() -> Void)?

    var takeID: UInt64 { lock.lock(); defer { lock.unlock() }; return take }
    /// Bumped on every session reset and take change: jobs started earlier are stale.
    var generation: UInt64 { lock.lock(); defer { lock.unlock() }; return generationValue }
    var version: UInt64 { lock.lock(); defer { lock.unlock() }; return versionValue }
    var count: Int { lock.lock(); defer { lock.unlock() }; return images.count }
    var gridStep: UInt64 { lock.lock(); defer { lock.unlock() }; return step }

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

    /// Grid spacing the UI currently wants (0 = none). Live capture only stores grid frames.
    func setStep(_ q: UInt64) {
        lock.lock()
        step = q
        if images.count > capacity, q > 0 {
            images = images.filter { $0.key % q == 0 }
            keysDirty = true
        }
        lock.unlock()
    }

    /// Cheap check for the emulation thread: is cursor frame f on the grid, on the cached take
    /// and not cached yet?
    func wants(frame f: UInt64, take t: UInt64) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return f > 0 && step > 0 && f % step == 0 && t == take && images[f] == nil
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

    func contains(_ f: UInt64) -> Bool { lock.lock(); defer { lock.unlock() }; return images[f] != nil }

    func missing(_ frames: [UInt64]) -> [UInt64] {
        lock.lock(); defer { lock.unlock() }
        return frames.filter { images[$0] == nil }
    }

    /// Image at frame f, or the cached frame nearest to f within `tolerance` frames.
    func image(near f: UInt64, tolerance: UInt64) -> CGImage? {
        lock.lock(); defer { lock.unlock() }
        if let img = images[f] { return img }
        if keysDirty { sortedKeys = images.keys.sorted(); keysDirty = false }
        guard !sortedKeys.isEmpty else { return nil }
        // First key >= f.
        var lo = 0, hi = sortedKeys.count
        while lo < hi {
            let mid = (lo + hi) / 2
            if sortedKeys[mid] < f { lo = mid + 1 } else { hi = mid }
        }
        var best: UInt64?
        var bestDist = UInt64.max
        for i in [lo - 1, lo] where i >= 0 && i < sortedKeys.count {
            let k = sortedKeys[i]
            let d = k > f ? k - f : f - k
            if d < bestDist { best = k; bestDist = d }
        }
        guard let k = best, bestDist <= tolerance else { return nil }
        return images[k]
    }
}
