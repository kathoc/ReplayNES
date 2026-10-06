// Filmstrip thumbnails for the timeline (main-thread coordinator + background generator).
// Images come from (1) frames the emulation thread displays anyway (live capture in
// EmulationController, O(1) check per step) and (2) a background job that replays the active
// take on its own core (rn_renderer, from power-on, faster than real time) and keeps only the
// grid frames that are still missing. The live session is never touched by the generator except
// for creating the renderer snapshot on the emulation thread.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import SwiftUI

/// One background generation run (cancellable from any thread).
final class ThumbnailJob {
    let targets: [UInt64]   // sorted cursor frames to make
    let take: UInt64
    let generation: UInt64
    private let lock = NSLock()
    private var cancelled = false

    init(targets: [UInt64], take: UInt64, generation: UInt64) {
        self.targets = targets
        self.take = take
        self.generation = generation
    }

    func cancel() { lock.lock(); cancelled = true; lock.unlock() }
    var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelled }
}

final class FilmstripModel: ObservableObject {
    static let shared = FilmstripModel()

    /// Bumped (throttled, <= 10 Hz) when thumbnails change: the timeline redraws.
    @Published private(set) var version: UInt64 = 0
    /// A/B slot edited on the timeline (drag to select, 「Aをここに」/「Bをここに」).
    @Published var selectedSlot = 0

    let cache: ThumbnailCache
    private let queue = DispatchQueue(label: "ReplayNES.thumbnails", qos: .userInitiated)
    private let notifyLock = NSLock()
    private var notifyQueued = false
    private var job: ThumbnailJob?
    private var target: (step: UInt64, length: UInt64)?
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

    /// Frames per filmstrip tile (ThumbnailGrid.tileStep, with hysteresis). 0 = not laid out yet.
    @Published private(set) var tileStep: UInt64 = 0
    /// While the tile step changes: the previous step, drawn on top and fading out.
    @Published private(set) var fade: (from: UInt64, start: Date)?
    static let fadeDuration = 0.3
    private var fadeEnd: DispatchWorkItem?

    /// The timeline's size or the take changed (main thread). Picks the tile step (= thumbnail
    /// grid) and schedules generation of whatever is missing.
    func layout(width: Double, tileWidth: Double, takeLength: UInt64) {
        let q = ThumbnailGrid.tileStep(width: width, tileWidth: tileWidth, length: takeLength,
                                       current: tileStep == 0 ? nil : tileStep)
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
        let missing = cache.missing(ThumbnailGrid.frames(length: t.length, step: t.step))
        let gen = cache.generation
        if let j = job, !j.isCancelled {
            // Keep a running job while it still covers everything that is missing.
            if j.generation == gen, Set(j.targets).isSuperset(of: missing) { return }
            j.cancel()
            job = nil
        }
        guard let first = missing.first, let last = missing.last else { return }
        let j = ThumbnailJob(targets: missing, take: cache.takeID, generation: gen)
        job = j
        // Renderer output index i = input record i = the picture at cursor i+1.
        let start = first - 1, end = last
        let cache = self.cache
        let queue = self.queue
        AppModel.shared.emu.perform { [weak self] e in
            guard let s = e.session, !j.isCancelled, s.activeTake == j.take, s.takeLength >= end,
                  let r = try? s.makeRenderer(start: start, end: end) else {
                DispatchQueue.main.async { self?.jobEnded(j, failed: true) }
                return
            }
            queue.async {
                Self.run(j, renderer: r, cache: cache)
                rn_renderer_free(r)
                DispatchQueue.main.async { self?.jobEnded(j, failed: false) }
            }
        }
    }

    private func jobEnded(_ j: ThumbnailJob, failed: Bool) {
        if job === j { job = nil }
        if failed && !j.isCancelled { retryAfter = Date().addingTimeInterval(2) }
        scheduleReconcile(after: 0.3)
    }

    /// Generator thread: replays the snapshot and keeps the target frames.
    private static func run(_ j: ThumbnailJob, renderer r: OpaquePointer, cache: ThumbnailCache) {
        var pending = Set(j.targets)
        var video: UnsafePointer<UInt32>?
        var audio: UnsafePointer<Int16>?
        var n = 0
        var idx: UInt64 = 0
        while !pending.isEmpty && !j.isCancelled {
            guard rn_renderer_next(r, &video, &audio, &n, &idx) == RN_OK else { return }
            let f = idx + 1
            guard pending.remove(f) != nil, !cache.contains(f), let v = video,
                  let img = ThumbnailScaler.thumbnail(v) else { continue }
            if !cache.insert(frame: f, take: j.take, generation: j.generation, image: img) { return } // stale
        }
    }
}
