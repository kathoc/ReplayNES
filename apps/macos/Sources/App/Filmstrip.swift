// Filmstrip thumbnails for the timeline (main-thread coordinator + background generator).
// Images come from (1) frames the emulation thread displays anyway (live capture in
// EmulationController, O(1) check per step) and (2) a background batch that renders just the
// missing pictures: one renderer per picture (rn_renderer seeded from the session's nearest
// checkpoint, so at most a few hundred frames of emulation each), run in parallel at utility QoS
// off the emulation thread. A batch is committed to the cache in one go, so reopening a project
// shows the whole filmstrip at once instead of tile by tile. The emulation thread only creates the
// renderers (a few copies each, a handful per tick).
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import SwiftUI

/// One background batch (cancellable from any thread).
final class ThumbnailJob {
    let targets: [UInt64]   // sorted cursor frames to make
    let take: UInt64
    let generation: UInt64
    private let lock = NSLock()
    private var cancelled = false
    private var failed = false
    private var pending: Int
    private var results: [(UInt64, CGImage)] = []

    init(targets: [UInt64], take: UInt64, generation: UInt64) {
        self.targets = targets
        self.take = take
        self.generation = generation
        pending = targets.count
    }

    func cancel() { lock.lock(); cancelled = true; lock.unlock() }
    var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelled }
    var didFail: Bool { lock.lock(); defer { lock.unlock() }; return failed }

    /// One target finished (image nil: not made). Returns true for the last one.
    func finish(_ f: UInt64, _ image: CGImage?, failed fail: Bool = false) -> Bool {
        lock.lock(); defer { lock.unlock() }
        if let image { results.append((f, image)) }
        if fail { failed = true }
        pending -= 1
        return pending == 0
    }

    var images: [(UInt64, CGImage)] { lock.lock(); defer { lock.unlock() }; return results }
}

final class FilmstripModel: ObservableObject {
    static let shared = FilmstripModel()
    /// -filmstripThumbnails NO: no thumbnails at all (pacing measurements, docs/FRAME_PACING.md).
    static let thumbnailsEnabled = UserDefaults.standard.flag("filmstripThumbnails", default: true)

    /// Bumped (throttled, <= 10 Hz) when thumbnails change: the timeline redraws.
    @Published private(set) var version: UInt64 = 0
    /// A/B slot edited on the timeline (drag to select, "Set A Here" / "Set B Here").
    @Published var selectedSlot = 0

    let cache: ThumbnailCache
    /// Renders the pictures of a batch in parallel, below emulation / display.
    private let renderQueue: OperationQueue = {
        let q = OperationQueue()
        q.name = "ReplayNES.thumbnails"
        q.qualityOfService = .utility
        q.maxConcurrentOperationCount = max(1, ProcessInfo.processInfo.activeProcessorCount - 2)
        return q
    }()
    /// Renderers created per emulation tick (each is a few small copies).
    private static let renderersPerTick = 8
    private let notifyLock = NSLock()
    private var notifyQueued = false
    private var job: ThumbnailJob?
    private var target: (step: Double, length: UInt64)?
    private var reconcileQueued = false
    private var retryAfter: Date = .distantPast

    private init() {
        cache = AppModel.shared.emu.thumbnails
        cache.onChange = { [weak self] in self?.queueNotify() }
    }

    private func queueNotify() {
        notifyLock.lock()
        if notifyQueued { notifyLock.unlock(); return }
        notifyQueued = true
        notifyLock.unlock()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
            guard let self else { return }
            self.notifyLock.lock(); self.notifyQueued = false; self.notifyLock.unlock()
            let v = self.cache.version
            if v != self.version { self.version = v }
        }
    }

    /// Frames per filmstrip tile = the timeline scale (ThumbnailGrid.tileStep). 0 = not laid out yet.
    @Published private(set) var tileStep: Double = 0
    /// While the tile step changes: the previous step, drawn on top and fading out.
    @Published private(set) var fade: (from: Double, start: Date)?
    static let fadeDuration = 0.3
    private var fadeEnd: DispatchWorkItem?

    /// The timeline's size or the take changed (main thread). Picks the tile step (= thumbnail
    /// grid) and schedules generation of whatever is missing.
    func layout(width: Double, tileWidth: Double, takeLength: UInt64) {
        let q = ThumbnailGrid.tileStep(width: width, tileWidth: tileWidth, length: takeLength)
        if q != tileStep {
            if tileStep != 0 && takeLength > 0 && (q == tileStep * 2 || q * 2 == tileStep) {
                fade = (tileStep, Date())
                fadeEnd?.cancel()
                let w = DispatchWorkItem { [weak self] in self?.fade = nil }
                fadeEnd = w
                DispatchQueue.main.asyncAfter(deadline: .now() + Self.fadeDuration, execute: w)
            } else {
                fade = nil
            }
            tileStep = q
        }
        cache.setStep(q)
        guard Self.thumbnailsEnabled else { return }
        target = (q, takeLength)
        scheduleReconcile(after: 0.3)
    }

    /// Throttled (not debounced: the take grows every frame while recording).
    private func scheduleReconcile(after delay: Double) {
        guard !reconcileQueued else { return }
        reconcileQueued = true
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
            self?.reconcileQueued = false
            self?.reconcile()
        }
    }

    private func reconcile() {
        guard let t = target else { return }
        if Date() < retryAfter { scheduleReconcile(after: 1); return }
        let missing = cache.missing(ThumbnailGrid.targets(length: t.length, step: t.step))
        let gen = cache.generation
        if let j = job, !j.isCancelled {
            // A running batch of this generation is short: let it finish (its pictures stay valid
            // while the take grows or the step changes); jobEnded reconciles the rest. Only a
            // take change / reset makes it stale.
            if j.generation == gen { return }
            j.cancel()
            job = nil
        }
        guard !missing.isEmpty else { return }
        let j = ThumbnailJob(targets: missing, take: cache.takeID, generation: gen)
        job = j
        createRenderers(j, from: 0)
    }

    /// Emulation thread, a few per tick: renderer for picture f = output index f - 1.
    private func createRenderers(_ j: ThumbnailJob, from i: Int) {
        AppModel.shared.emu.perform { [weak self] e in
            guard let self else { return }
            let end = min(i + Self.renderersPerTick, j.targets.count)
            for f in j.targets[i..<end] {
                guard let s = e.session, !j.isCancelled, s.activeTake == j.take, f <= s.takeLength,
                      let r = try? s.makeRenderer(start: f - 1, end: f) else {
                    if j.finish(f, nil, failed: !j.isCancelled) { self.batchDone(j) }
                    continue
                }
                self.renderQueue.addOperation { [weak self] in
                    var img: CGImage?
                    if !j.isCancelled {
                        var video: UnsafePointer<UInt32>?
                        var audio: UnsafePointer<Int16>?
                        var n = 0
                        var idx: UInt64 = 0
                        if rn_renderer_next(r, &video, &audio, &n, &idx) == RN_OK, idx + 1 == f, let v = video {
                            img = ThumbnailScaler.thumbnail(v)
                        }
                    }
                    rn_renderer_free(r)
                    if j.finish(f, img, failed: img == nil && !j.isCancelled) { self?.batchDone(j) }
                }
            }
            if end < j.targets.count { self.createRenderers(j, from: end) }
        }
    }

    /// Any thread: the whole batch is in (or cancelled). Commits what was made in one update.
    private func batchDone(_ j: ThumbnailJob) {
        cache.insert(batch: j.images, take: j.take, generation: j.generation)  // rejected if stale
        DispatchQueue.main.async { [weak self] in self?.jobEnded(j, failed: j.didFail) }
    }

    private func jobEnded(_ j: ThumbnailJob, failed: Bool) {
        if job === j { job = nil }
        if failed && !j.isCancelled { retryAfter = Date().addingTimeInterval(2) }
        scheduleReconcile(after: 0.3)
    }
}
