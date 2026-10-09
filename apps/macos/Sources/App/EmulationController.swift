// Emulation thread: the only owner of the rn_session.
// The display decides only WHEN rn_step is called (pacing, pause, slow, advance, rewind);
// WHAT is emulated is fully determined by the recorded input/event stream.
// Pacing (docs/FRAME_PACING.md): a CAMetalDisplayLink on this thread's run loop calls back once
// per display refresh. Refreshes that start a frame (DisplayCadence: every 2nd at 120 Hz) wait
// until just before the commit deadline (InputDeadline), sample input, emulate one frame, render
// and present it for that refresh, then do housekeeping (autosave, status). Other refreshes
// present the same picture again (steady cadence). Without a visible viewport the host clock
// paces at the NTSC rate.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import QuartzCore

/// The viewport the display link presents to (created on the main thread by GameLayerView).
final class DisplayTarget {
    let layer: CAMetalLayer
    let renderer: GameRenderer
    let maxFPS: Int   // the screen's highest refresh rate (120 on ProMotion)
    let variableRefresh: Bool   // ProMotion / Adaptive-Sync (NSScreen min != max refresh interval)
    init(layer: CAMetalLayer, renderer: GameRenderer, maxFPS: Int, variableRefresh: Bool) {
        self.layer = layer
        self.renderer = renderer
        self.maxFPS = maxFPS
        self.variableRefresh = variableRefresh
    }
}

/// Presentation feedback from Metal's presented handlers to the emulation thread: detects a
/// steady backlog (every present a whole refresh behind its target: one drawable too many in the
/// queue, which a constant present-per-refresh cadence never drains by itself) and latch misses.
private final class PresentFeedback {
    private let lock = NSLock()
    private var lateRun = 0
    private var misses = 0
    func presented(late: Bool) { lock.lock(); lateRun = late ? lateRun + 1 : 0; lock.unlock() }
    /// A new frame's own present was dropped or late while the present before it was on time (not
    /// a backlog): direct to the display, its commit missed the window server's latch.
    func newFrameMissed() { lock.lock(); if lateRun == 0 { misses += 1 }; lock.unlock() }
    func takeMisses() -> Int { lock.lock(); defer { lock.unlock() }; let n = misses; misses = 0; return n }
    /// True (once) when the last `n` presents were all late.
    func takeSteadyBacklog(_ n: Int) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard lateRun >= n else { return false }
        lateRun = 0
        return true
    }
}

/// CAMetalDisplayLinkDelegate for the emulation thread (EmulationController is not an NSObject).
private final class DisplayLinkProxy: NSObject, CAMetalDisplayLinkDelegate {
    weak var owner: EmulationController?
    init(owner: EmulationController) { self.owner = owner }
    func metalDisplayLink(_ link: CAMetalDisplayLink, needsUpdate update: CAMetalDisplayLink.Update) {
        guard let owner else { return }
        if owner.isEmulationThread {
            autoreleasepool { owner.displayRefresh(update) }
            return
        }
        // Core Animation sometimes delivers an update on the main thread instead of the run loop
        // the link was added to: AppKit's transaction flush dispatches "deferred" display links
        // (stepTransactionFlush -> CA::Display::DisplayLink::dispatch_deferred_display_links), e.g.
        // on every key event in full screen. The emulation state is owned by the emulation thread,
        // so the update is handed to it (microseconds later) instead of being dropped: a dropped
        // update cost that refresh (judder) and, on a frame refresh, half a frame of emulated
        // time (audio underruns at a high key rate).
        owner.latency.recordForeignCallback()
        owner.forwardDisplayUpdate(update, from: link)
    }
}

/// UI-facing snapshot of the emulation state (published ~20 Hz).
struct EmuStatus: Equatable {
    var hasSession = false
    var frame: UInt64 = 0
    var takeLength: UInt64 = 0
    var recording = true
    var paused = true
    var slow: SlowRate = .normal
    var rewinding = false
    var fastForward = false
    var endOfTake = false
    var activeTake: UInt64 = 0
    var takeCount = 0
    var undoDepth = 0
    var unsaved = false
    var projectPath = ""
    var romPath = ""
    var advancePending = 0
    var flashActive = false   // the flash reduction filter changed the picture recently
    // Practice (A/B repeat). frame/takeLength above stay the take's (frozen while practicing).
    var practicing = false
    var practiceSlot = -1            // slot being looped (-1 = none / free practice)
    var practiceFrame: UInt64 = 0    // frames since A
    var practiceLength: UInt64 = 0   // slot length A->B (0 = no B: no loop)
    var practiceLooping = false      // hold / rewind animation in progress
    var practiceLoops = 0            // completed A->B loops since entering
}

/// Bookmarks, takes and practice slots (published when they change).
struct SessionStructure: Equatable {
    var bookmarks: [BookmarkInfo] = []
    var takes: [TakeInfo] = []
    var practiceSlots: [PracticeSlotInfo] = []
}

final class EmulationController {
    let frames = FrameBuffer()
    let audio = AudioOutput()
    let latency = LatencyMeter()
    /// Filmstrip thumbnails of the active take (see Filmstrip.swift / TimelineActions.swift).
    let thumbnails = ThumbnailCache()
    let input: InputManager

    // Callbacks, always invoked on the main thread.
    var onStatus: ((EmuStatus) -> Void)?
    var onStructure: ((SessionStructure) -> Void)?
    var onError: ((String, String) -> Void)?   // title, message
    var onNotice: ((String) -> Void)?

    private var thread: Thread?
    private let cmdLock = NSLock()
    private var commands: [(EmulationController) -> Void] = []
    private var running = true

    // ---- emulation-thread state (never touched from other threads) ----
    private(set) var session: EngineSession?
    var paused = true {
        didSet {
            if paused != oldValue { statusDirty = true; if !paused { pauseHintShown = false; stepRepeater.reset() } }
            if paused && !oldValue { autosaveSoon = true } // pausing persists right away (resume)
            input.setPausedStepMode(paused)
        }
    }
    var slow: SlowRate = .normal { didSet { statusDirty = true } }
    var advanceRemaining = 0
    var uiRewindHeld = false
    var uiFastForwardHeld = false
    var pendingEvents: UInt8 = 0
    var scrubTarget: UInt64?
    var pauseAfterRewind = false
    var autosaveInterval: Double = 5
    /// The session is the temporary project (SessionResume.swift): when paused it is also folded
    /// into a full save now and then, so a resume after a force quit starts from recent checkpoints.
    var tempSession = false
    private var rewinding = false
    private var fastForward = false
    private var rewindTicks = 0
    private var ffTicks = 0   // ticks of the fast-forward hold (rnf_hold_speed)
    private var tickCount: UInt64 = 0
    private var statusDirty = true
    private var structureDirty = true
    private var endOfTake = false
    private var cachedTakeCount = 0
    private var lastAutosave: UInt64 = 0
    private var lastPressSeq: UInt64 = 0
    private var pauseHintShown = false
    private var autosaveFailed = false
    private var autosaveSoon = false
    private var lastFullSave: UInt64 = 0
    private var nextDeadline: UInt64 = 0
    // Fast-forward (replays the recorded take only; never records).
    private var ff = FastForwardSession()
    var fastForwardActive: Bool { ff.active }
    private var ffBlocked = false           // hold started where nothing is recorded ahead
    private var pausedBeforeFF = true
    // Paused D-pad stepping.
    private var stepRepeater = StepRepeater()
    // Practice.
    private var practiceSlot: Int?          // slot being looped
    private var practiceLength: UInt64 = 0  // its A->B length (0 = no B)
    private var practiceLoop = PracticeLoop()
    private var practiceSeq: UInt64 = 1 << 40 // input sampling clock while practicing (turbo phase)
    private var modeBeforePractice = RN_MODE_RECORD
    private let history = FrameHistory(capacity: 60)
    private var practiceRewindStarted = false
    // Photosensitive flash reduction (display only: filters the copy that is shown).
    private let flashFilter = FlashFilter(level: .standard)
    private var displayFrame = [UInt32](repeating: 0xFF00_0000, count: Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT))
    private var flashAltered = false        // last shown frame differs from the emulated one
    private var lastFlashTick: UInt64 = 0   // tick of the last altered frame
    private var flashActiveShown = false
    private let period: UInt64 = HostClock.ticks(seconds: Double(RN_FPS_DEN) / Double(RN_FPS_NUM))
    // Display-locked pacing (emulation thread).
    private var link: CAMetalDisplayLink?
    private var linkProxy: DisplayLinkProxy?
    private var linkTarget: DisplayTarget?
    private var linkActive = false
    private var lastLinkCallback: UInt64 = 0
    private var cadence = DisplayCadence()
    private var inputDeadline = InputDeadline()
    private let feedback = PresentFeedback()
    private var tickStart: UInt64 = 0           // stamped on frames (latency breakdown)
    private var lastSample: UInt64 = 0          // input sample of the last emulated frame
    private var lastStep: UInt64 = 0            // when a frame was last emulated (repeat presents)
    private var emulationRate = 1 / FramePacing.period
    private var pendingThumbnail: UInt64?       // captured after the frame is presented
    private var lastDrain: UInt64 = 0
    private var lastUpdateTarget = 0.0          // targetPresentationTimestamp of the last update handled
    private var presentPath = PresentPath()
    private var runLoop: CFRunLoop?             // the emulation thread's (targetLock)
    /// Deadline hint for the frame burst (rn_frame_workgroup.h); -frameWorkgroup NO disables it.
    private var workgroup: OpaquePointer?
    static let useWorkgroup = UserDefaults.standard.flag("frameWorkgroup", default: true)
    /// -repeatPresents NO: present only new frames (every k-th refresh) instead of every refresh.
    static let repeatPresents = UserDefaults.standard.flag("repeatPresents", default: true)
    // Experiment knobs (docs/FRAME_PACING.md): -backlogDrain NO, -latchFeedback NO, -inputLeadMs <ms> (fixed lead).
    static let backlogDrain = UserDefaults.standard.flag("backlogDrain", default: true)
    /// -latchFeedback NO: dropped / late presents on the direct path do not raise the input lead.
    static let latchFeedback = UserDefaults.standard.flag("latchFeedback", default: true)
    static let fixedLead: Double? = UserDefaults.standard.object(forKey: "inputLeadMs") != nil
        ? UserDefaults.standard.double(forKey: "inputLeadMs") / 1000 : nil
    // Requested viewport (any thread -> emulation thread).
    private let targetLock = NSLock()
    private var requestedTarget: DisplayTarget?
    private var targetChanged = false


    init(input: InputManager) {
        self.input = input
    }

    func start() {
        audio.start()
        let t = Thread { [weak self] in self?.threadMain() }
        t.name = "ReplayNES.emulation"
        t.qualityOfService = .userInteractive
        t.stackSize = 4 << 20
        thread = t
        t.start()
    }

    // MARK: command queue

    /// Runs `block` on the emulation thread at the start of the next tick (<= 1 frame).
    func perform(_ block: @escaping (EmulationController) -> Void) {
        cmdLock.lock(); commands.append(block); cmdLock.unlock()
    }

    /// Runs `block` on the emulation thread and waits (main thread use only; emulation thread
    /// never waits on main, so this cannot deadlock).
    func sync<T>(timeout: Double = 30, _ block: @escaping (EmulationController) -> T) -> T? {
        let sem = DispatchSemaphore(value: 0)
        var out: T?
        perform { emu in out = block(emu); sem.signal() }
        return sem.wait(timeout: .now() + timeout) == .success ? out : nil
    }

    /// Stops the emulation thread and releases the session (closing its files) on that thread
    /// before returning, so quitting - including Sparkle's install-and-relaunch - never races
    /// with an in-flight frame or autosave.
    func shutdown() {
        _ = sync(timeout: 10) { emu in
            emu.running = false
            emu.session = nil
        }
    }

    private func onMain(_ f: @escaping () -> Void) { DispatchQueue.main.async(execute: f) }

    func reportError(_ title: String, _ error: Error) {
        let msg = (error as? LocalizedError)?.errorDescription ?? "\(error)"
        onMain { [weak self] in self?.onError?(title, msg) }
    }

    func notice(_ text: String) { onMain { [weak self] in self?.onNotice?(text) } }

    // MARK: session lifecycle (emulation thread)

    func install(_ s: EngineSession?) {
        session = s
        // A fresh recording (empty take, frame 0) has nothing to review: run immediately so the
        // game boots. Opened projects with recorded input stay paused at their cursor.
        if let s, s.takeLength == 0, s.frame == 0, s.mode == RN_MODE_RECORD {
            paused = false
        } else {
            paused = true
        }
        slow = .normal
        advanceRemaining = 0
        scrubTarget = nil
        pendingEvents = 0
        endOfTake = false
        lastPressSeq = input.pressSequence
        pauseHintShown = false
        audio.setMuted(true)
        lastAutosave = HostClock.now()
        lastFullSave = 0
        autosaveFailed = false
        structureDirty = true
        statusDirty = true
        resetFlashFilter()
        ff = FastForwardSession()
        ffBlocked = false
        stepRepeater.reset()
        practiceSlot = nil
        practiceLength = 0
        practiceLoop.reset()
        history.release()
        thumbnails.reset(take: s?.activeTake ?? 0)
        if let s, s.mode == RN_MODE_PRACTICE { try? s.setMode(RN_MODE_RECORD) } // never persisted; defensive
        if let s, let v = s.video { show(v, meta: FrameMeta(frame: s.frame)) } else { frames.clear() }
        publishStatus(force: true)
    }

    func markStructureDirty() { structureDirty = true; statusDirty = true }

    /// Shows the session's current frame. `continuous` = it directly follows the previously shown
    /// frame (playback, rewind steps); otherwise it is a jump and the flash filter is reset first so
    /// unrelated pictures are never blended.
    func publishVideo(continuous: Bool = false) {
        guard let s = session, let v = s.video else { return }
        if !continuous { resetFlashFilter() }
        show(v, meta: FrameMeta(frame: s.frame, emulatedTime: HostClock.now()))
        statusDirty = true
    }

    // MARK: flash reduction (emulation thread)

    func setFlashLevel(_ l: FlashLevel) {
        guard l != flashFilter.level else { return }
        flashFilter.setLevel(l)
        flashAltered = false
        if l == .off { publishVideo() } // show the unfiltered picture right away
    }

    func resetFlashFilter() {
        flashFilter.reset()
        flashAltered = false
    }

    /// Hands a frame to the display, through the flash filter unless it is off. The session's own
    /// buffer is only read; the filtered copy is what the viewport (and snapshots) show.
    private func show(_ v: UnsafePointer<UInt32>, meta: FrameMeta) {
        // Raw PPU codes of the same picture for the CRT signal path (display only).
        var meta = meta
        meta.tickStart = tickStart
        let signal = session?.videoIndices
        if let signal { meta.hasCodes = true; meta.burstPhase = signal.burst_phase; meta.signalFrame = signal.frame }
        if flashFilter.level == .off {
            frames.publish(v, meta: meta, codes: signal?.codes)
            return
        }
        let altered = displayFrame.withUnsafeMutableBufferPointer { flashFilter.process(v, into: $0.baseAddress!) }
        flashAltered = altered
        if altered { lastFlashTick = tickCount }
        meta.flashAltered = altered
        displayFrame.withUnsafeBufferPointer { frames.publish($0.baseAddress!, meta: meta, codes: signal?.codes) }
    }

    /// Seek helper used by commands: pauses, mutes, refreshes the picture.
    func seekCommand(_ f: UInt64) {
        guard let s = session else { return }
        if s.mode == RN_MODE_PRACTICE {
            // The take cursor is frozen while practicing: "Go to Start" means back to A.
            if f == 0, let slot = practiceSlot { startPractice(slot) } else { notice(Self.practiceBlockedText) }
            return
        }
        if ff.active { endFastForward(s) }
        do {
            try s.seek(min(f, s.takeLength))
            endOfTake = false
        } catch { reportError(String(localized: "Couldn’t move"), error) }
        paused = true
        audio.setMuted(true)
        publishVideo()
        captureThumbnail(s, frame: s.frame)
    }

    // MARK: thread loop

    private var emulationThread: pthread_t?
    fileprivate var isEmulationThread: Bool { emulationThread.map { pthread_equal($0, pthread_self()) != 0 } ?? false }

    private func threadMain() {
        emulationThread = pthread_self()
        targetLock.lock(); runLoop = CFRunLoopGetCurrent(); targetLock.unlock()
        // Real-time (time-constraint) thread, woken every display refresh (8.3 ms at 120 Hz); a frame needs ~1 ms of CPU.
        HostClock.makeCurrentThreadRealtime(period: FramePacing.period / 2, computation: 0.003, constraint: 0.007)
        if Self.useWorkgroup { workgroup = rn_frame_workgroup_join_new("ReplayNES.frame") }
        defer { rn_frame_workgroup_leave_free(workgroup); workgroup = nil }
        nextDeadline = HostClock.now()
        while running {
            syncDisplayTarget()
            if link != nil && linkActive {
                // Display callbacks (displayRefresh) run the frames.
                _ = CFRunLoopRunInMode(.defaultMode, 0.05, true)
                if HostClock.seconds(HostClock.now() &- lastLinkCallback) > 0.1 {
                    // Window hidden / occluded / minimised: keep emulating on the host clock.
                    linkActive = false
                    nextDeadline = HostClock.now()
                }
            } else {
                if link != nil {
                    _ = CFRunLoopRunInMode(.defaultMode, 0, true)  // a callback takes over again
                    if linkActive { continue }
                }
                hostTick()
            }
        }
        link?.invalidate()
        link = nil
        // Thread exit: release the session on its own thread.
        session = nil
    }

    /// One frame on the host clock at the NTSC rate (no visible viewport: window hidden, occluded
    /// or minimised; nothing is presented then).
    private func hostTick() {
        let now = HostClock.now()
        if now < nextDeadline { mach_wait_until(nextDeadline) }
        let woke = HostClock.now()
        latency.recordTickWake(lateTicks: woke > nextDeadline ? woke - nextDeadline : 0)
        tickStart = woke
        emulationRate = 1 / FramePacing.period
        runCommands()
        rn_frame_workgroup_start(workgroup, HostClock.now(), nextDeadline &+ period / 2)
        let s = tickFrame()
        rn_frame_workgroup_finish(workgroup)
        let after = HostClock.now()
        afterTick(s, slack: nextDeadline &+ period > after ? HostClock.seconds(nextDeadline &+ period - after) : 0)
        nextDeadline &+= period
        let end = HostClock.now()
        if end > nextDeadline &+ period * 6 {
            // Stalled (app nap, debugger, heavy seek): resync. Never "catch up" by stepping
            // extra frames for wall-clock reasons.
            latency.recordLateTick()
            nextDeadline = end
        }
    }

    // MARK: display link (emulation thread)

    /// Main thread (GameLayerView): the viewport to present to.
    func setDisplayTarget(_ t: DisplayTarget) {
        targetLock.lock(); requestedTarget = t; targetChanged = true; targetLock.unlock()
    }

    func clearDisplayTarget(_ t: DisplayTarget) {
        targetLock.lock()
        if requestedTarget === t { requestedTarget = nil; targetChanged = true }
        targetLock.unlock()
    }

    private func syncDisplayTarget() {
        targetLock.lock()
        let changed = targetChanged
        let t = requestedTarget
        targetChanged = false
        targetLock.unlock()
        guard changed, t !== linkTarget else { return }
        link?.invalidate()
        link = nil
        linkTarget = t
        linkActive = false
        guard let t else { return }
        let l = CAMetalDisplayLink(metalLayer: t.layer)
        let proxy = DisplayLinkProxy(owner: self)
        l.delegate = proxy
        // As many refreshes as the display offers (120 Hz ProMotion): a frame is emulated on
        // every k-th one (DisplayCadence) and the picture re-presented on the others.
        let maxFPS = Float(max(60, t.maxFPS))
        l.preferredFrameRateRange = CAFrameRateRange(minimum: maxFPS, maximum: maxFPS, preferred: maxFPS)
        l.preferredFrameLatency = 1
        l.add(to: .current, forMode: .default)
        link = l
        linkProxy = proxy
        lastUpdateTarget = 0
        presentPath.reset()
        cadence = DisplayCadence()
        nextDeadline = HostClock.now()
    }

    /// Main thread: a display link update delivered there (DisplayLinkProxy) runs on the
    /// emulation thread's run loop instead, unless the link was replaced meanwhile.
    fileprivate func forwardDisplayUpdate(_ update: CAMetalDisplayLink.Update, from link: CAMetalDisplayLink) {
        targetLock.lock(); let rl = runLoop; targetLock.unlock()
        guard let rl else { return }
        CFRunLoopPerformBlock(rl, CFRunLoopMode.defaultMode.rawValue) { [weak self] in
            guard let self, self.link === link else { return }
            autoreleasepool { self.displayRefresh(update) }
        }
        CFRunLoopWakeUp(rl)
    }

    /// CAMetalDisplayLink callback (emulation thread run loop): one display refresh.
    fileprivate func displayRefresh(_ update: CAMetalDisplayLink.Update) {
        // A forwarded update can arrive after a later one: never present out of order.
        guard update.targetPresentationTimestamp > lastUpdateTarget else { return }
        lastUpdateTarget = update.targetPresentationTimestamp
        let cb = HostClock.now()
        lastLinkCallback = cb
        linkActive = true
        guard let target = linkTarget else { return }
        let present = update.targetPresentationTimestamp
        let newFrame = cadence.refresh(presentation: present)
        let refresh = cadence.refresh > 0 ? cadence.refresh : 1.0 / 120
        let fb = feedback, lat = latency
        for _ in 0..<fb.takeMisses() { inputDeadline.observeMiss() }
        // Direct to the display (presentDelay = 1 refresh) a new frame whose present is dropped or
        // late missed the window server's latch: under load (other apps keeping the window server
        // busy) the latch comes earlier, so these raise the input lead like late commits do
        // (measured under such load: drops 225 -> 52 -> 25 per 25 s with a lead of 2.5 -> 4 -> 6
        // ms). Composited, drops do not depend on the lead; a picture built ahead (BuildAhead) is
        // committed a refresh early anyway.
        presentPath.observe(presentDelay: present - update.targetTimestamp)
        let direct = presentPath.isDirect(refresh: refresh)
        let latchFeedback = Self.latchFeedback && direct && !target.renderer.pipelined
        let onRepeat: (UInt64, Double) -> Void = { frame, t in
            // A whole refresh late = behind (a ProMotion panel in full screen may run its refreshes
            // a fraction of a refresh off the display link's timestamps; that is not).
            fb.presented(late: t > present + refresh * 0.75)
            lat.recordRepeatPresented(frame: frame, target: present, presentedSeconds: t)
        }
        // The first present of a new frame's picture (this refresh, or the next one when the
        // picture is built a refresh ahead: GameRenderer.pipelined).
        let onNew: (FrameMeta, UInt64, Double, GPUSpan) -> Void = { meta, commit, t, gpu in
            // Dropped (never shown; a later present showed the picture a refresh late).
            if t <= 0 {
                lat.recordDropped()
                if latchFeedback { fb.newFrameMissed() }
                return
            }
            let late = lat.recordPresent(meta: meta, commit: commit, presentedSeconds: t, gpu: gpu.times)
            if late && latchFeedback { fb.newFrameMissed() }
            fb.presented(late: late)
        }
        // Presents all a refresh behind (BacklogDrain): skipping one present that would only
        // repeat the current picture drains the extra drawable.
        let drain = BacklogDrain.policy(variableRefresh: target.variableRefresh, direct: direct)
        let drainNow = Self.backlogDrain && lastStep != 0 && HostClock.seconds(cb &- lastStep) < 0.5
            && !target.renderer.hasPendingPicture && (!newFrame || target.renderer.pipelined)
            && HostClock.seconds(cb &- lastDrain) > drain.minInterval && fb.takeSteadyBacklog(drain.lateRun)
        if drainNow {
            lastDrain = cb
            latency.recordBacklogDrain()
        }
        guard newFrame else {
            let playing = Self.repeatPresents && lastStep != 0 && HostClock.seconds(cb &- lastStep) < 0.5
            if drainNow { return }
            if target.renderer.present(drawable: update.drawable, targetPresentation: present,
                                       presentDelay: present - update.targetTimestamp, newFrame: false,
                                       repeatPicture: playing, onPresented: onNew, onRepeatPresented: onRepeat) != nil,
               playing {
                latency.recordRepeat()
            }
            return
        }
        // Commands (seeks, take switches, ...) run before the wait: their cost is absorbed there.
        runCommands()
        let lead = Self.fixedLead ?? inputDeadline.lead
        HostClock.wait(untilSeconds: update.targetTimestamp - lead)
        rn_frame_workgroup_start(workgroup, HostClock.now(), HostClock.ticks(seconds: update.targetTimestamp))
        tickStart = cb
        emulationRate = cadence.emulationRate
        let stepsBefore = lastStep
        let s = tickFrame()
        let stepped = lastStep != stepsBefore
        let playing = Self.repeatPresents && lastStep != 0 && HostClock.seconds(HostClock.now() &- lastStep) < 0.5
        let commit = target.renderer.present(
            drawable: update.drawable, targetPresentation: present, presentDelay: present - update.targetTimestamp,
            newFrame: true, repeatPicture: playing,
            skipRepeat: drainNow, onPresented: onNew, onRepeatPresented: onRepeat)
        rn_frame_workgroup_finish(workgroup)
        // Committed (nearly) past the deadline: sample earlier from now on. (Presentation
        // lateness counts only through latchFeedback above: a queue backlog, a busy compositor or
        // variable refresh also delay frames that were committed in time.)
        if let commit, commit > HostClock.ticks(seconds: update.targetTimestamp - InputDeadline.lateCommit) {
            inputDeadline.observeMiss()
        }
        if stepped, let commit, commit > lastSample {
            // CPU work from the sample to the commit; the GPU part (a few µs for the plain picture,
            // more with the CRT model) is covered by the margin and the miss feedback (Metal's
            // GPU start/end times include waiting for the drawable, so they are not added).
            inputDeadline.observeWork(HostClock.seconds(commit - lastSample))
        }
        latency.recordCadence(pacing: cadence.refreshesPerFrame.map { "display link \(Int((1 / refresh).rounded())) Hz / \($0)" } ?? "display link \(Int((1 / refresh).rounded())) Hz (free)",
                              refresh: refresh, expectedInterval: cadence.expectedFrameInterval, inputLead: inputDeadline.lead)
        let now = HostClock.seconds(HostClock.now())
        afterTick(s, slack: update.targetTimestamp + cadence.expectedFrameInterval - inputDeadline.lead - now)
    }

    private func runCommands() {
        cmdLock.lock()
        let cmds = commands
        commands.removeAll(keepingCapacity: true)
        cmdLock.unlock()
        for c in cmds { c(self) }
    }

    /// The frame-critical part of a tick: hotkeys, input sample, emulation, publish. Returns the
    /// session when the housekeeping (autosave) may run afterwards.
    private func tickFrame() -> (EngineSession?, autosave: Bool) {
        tickCount &+= 1
        guard let s = session else { return (nil, false) }

        // Read the press counter before polling hotkeys so a hotkey press is never mistaken
        // for a game-button press below.
        let pressSeq = input.pressSequence
        var edges: UInt32 = 0, held: UInt32 = 0
        rn_input_poll_hotkeys(input.handle, &edges, &held)
        let wasPaused = paused
        if edges != 0 { handleHotkeys(edges, s) }
        if pressSeq != lastPressSeq {
            lastPressSeq = pressSeq
            // Game input is not emulated while paused; say so once instead of silently ignoring it.
            if wasPaused && paused && edges == 0 && held == 0 && advanceRemaining == 0 && !pauseHintShown && !uiRewindHeld {
                pauseHintShown = true
                notice(String(localized: "Paused. Press Space, or the back button on the controller, to resume"))
            }
        }

        let practicing = s.mode == RN_MODE_PRACTICE
        let wantRewind = uiRewindHeld || held & UInt32(RN_HK_REWIND) != 0
        let wantFF = !wantRewind && !practicing && (uiFastForwardHeld || held & UInt32(RN_HK_FAST_FORWARD) != 0)
        if wantRewind != rewinding || wantFF != fastForward { statusDirty = true }
        if wantFF && !fastForward { beginFastForward(s) }
        if !wantFF && fastForward { endFastForward(s) }
        fastForward = wantFF

        if wantRewind {
            if !rewinding { resetFlashFilter() } // rewind start: a new continuous (backwards) sequence
            rewinding = true
            if practicing { practiceLoop.interrupt() }
            audio.setMuted(true)
            rewindTicks += 1
            let n = UInt64(rnf_hold_speed(Int32(rewindTicks)))   // the same curve as fast-forward
            // Practice: rewinds the practice run only (the engine stops at A); take: clamps at 0.
            if practicing ? s.practiceStatus.rewindAvailable > 0 : s.frame > 0 {
                do {
                    try s.rewind(n)
                    endOfTake = false
                    publishVideo(continuous: true)
                } catch { reportError(String(localized: "Rewind failed"), error); uiRewindHeld = false }
            }
            return (s, false)
        }
        if rewinding {
            rewinding = false
            rewindTicks = 0
            if pauseAfterRewind { paused = true }
        }

        if let t = scrubTarget {
            scrubTarget = nil
            if !practicing { seekCommand(t) }
            return (s, false)
        }

        if paused {
            let d = stepRepeater.tick()
            if d != 0 { stepFrame(d, s) }
        }

        if fastForward {
            tickFastForward(s)
        } else if practicing && !paused {
            tickPractice(s)
        } else {
            var steps = 0
            var audible = false
            if paused {
                // Frame-advance users press buttons while paused on purpose: no hint for them.
                if advanceRemaining > 0 { steps = 1; advanceRemaining -= 1; statusDirty = true; pauseHintShown = true }
            } else {
                if tickCount % UInt64(slow.rawValue) == 0 { steps = 1 }
                audible = slow == .normal
            }
            audio.setMuted(!audible)
            var stepped = 0
            for _ in 0..<steps {
                // Paused steps are navigation: inside the take they replay it (never a new take).
                if !stepOnce(s, audible: audible, frameStep: paused) { break }
                stepped += 1
            }
            // A frame held back by the flash filter while nothing new is emulated (pause, slow
            // motion) is re-filtered every tick, so the picture settles on the real frame within the
            // filter's 1-second budget instead of staying blended.
            if stepped == 0 && flashAltered && !rewinding { publishVideo(continuous: true) }
        }
        let active = flashFilter.level != .off && tickCount &- lastFlashTick < 45 && lastFlashTick != 0
        if active != flashActiveShown { flashActiveShown = active; statusDirty = true }
        return (s, true)
    }

    /// After the frame is on its way: thumbnail, autosave (when `slack` seconds remain before the
    /// next frame's work must start), status for the UI.
    private func afterTick(_ r: (EngineSession?, autosave: Bool), slack: Double) {
        if let s = r.0, let f = pendingThumbnail {
            pendingThumbnail = nil
            captureThumbnail(s, frame: f)
        }
        if let s = r.0, r.autosave { maybeAutosave(s, slack: slack) }
        if r.0 == nil || tickCount % 3 == 0 || structureDirty { publishStatus(force: false) }
        latency.recordThreadCPU(emulation: HostClock.threadCPUSeconds())
    }

    // MARK: fast-forward (emulation thread)

    private func beginFastForward(_ s: EngineSession) {
        pausedBeforeFF = paused
        ffTicks = 0
        ffBlocked = s.frame >= s.takeLength
        if ffBlocked {
            notice(s.takeLength == 0 ? String(localized: "Nothing has been recorded yet, so there is nothing to fast-forward") : String(localized: "End of the recording (fast-forward stops here)"))
            return
        }
        do { try ff.begin(s) } catch { reportError(String(localized: "Couldn’t fast-forward"), error); ffBlocked = true }
        endOfTake = false
    }

    func endFastForward(_ s: EngineSession) {
        let wasActive = ff.active
        do { try ff.end(s) } catch { reportError(String(localized: "Couldn’t return to record mode"), error) }
        fastForward = false
        statusDirty = true
        if wasActive {
            // Like rewind: optionally stop where the user let go, so recording never resumes by surprise.
            paused = pauseAfterRewind ? true : (pausedBeforeFF || s.frame >= s.takeLength)
        }
        ffBlocked = false
    }

    /// Fast-forward replays recorded frames 2x -> 4x (rnf_hold_speed, silent) and stops at the take end, paused there.
    private func tickFastForward(_ s: EngineSession) {
        audio.setMuted(true)
        guard ff.active, !ffBlocked else { return }
        do {
            ffTicks += 1   // the same speed curve as rewind (rnf_hold_speed), forwards
            let (n, atEnd) = try ff.step(s, frames: Int(rnf_hold_speed(Int32(ffTicks))))
            if n > 0 { publishVideo(continuous: true) }
            if atEnd {
                ffBlocked = true
                paused = true
                notice(String(localized: "Reached the end of the recording (paused here)"))
            }
        } catch {
            ffBlocked = true
            paused = true
            reportError(String(localized: "Couldn’t fast-forward"), error)
        }
    }

    // MARK: practice (emulation thread)

    static let practiceBlockedText = String(localized: "Not available while practicing. Use “Stop Practicing” to return to the take first")

    var isPracticing: Bool { session?.mode == RN_MODE_PRACTICE }

    /// Practice loop: play A->B, hold the last frame 0.5 s (audio dies away), show the recent
    /// frames backwards for 0.5 s, then goto A and play again.
    private func tickPractice(_ s: EngineSession) {
        let now = HostClock.seconds(HostClock.now())
        let action = practiceLoop.tick(now: now, counter: s.practiceFrame,
                                       length: practiceSlot != nil && practiceLength > 0 ? practiceLength : nil)
        switch action {
        case .step:
            practiceRewindStarted = false
            let audible = slow == .normal
            audio.setMuted(!audible)
            if tickCount % UInt64(slow.rawValue) == 0 { stepOnce(s, audible: audible) }
            if flashAltered && tickCount % UInt64(slow.rawValue) != 0 { publishVideo(continuous: true) }
        case .beginHold:
            // Picture stays; a short decaying tail lets the sound end naturally (no click, no black).
            let tail = AudioFade.tail(from: s.audio())
            tail.withUnsafeBufferPointer { audio.push($0) }
            statusDirty = true
        case .hold:
            break
        case .rewindFrame(let back):
            if !practiceRewindStarted {
                practiceRewindStarted = true
                audio.setMuted(true)
                resetFlashFilter()
                statusDirty = true
            }
            let idx = PracticeLoop.historyIndex(back: back, count: history.count)
            _ = history.withFrame(back: idx) { show($0, meta: FrameMeta(frame: s.frame, emulatedTime: HostClock.now())) }
        case .restart:
            practiceRewindStarted = false
            guard let slot = practiceSlot else { return }
            do {
                try s.practiceGotoA(slot)
                history.clear()
                // No publish here: rn_video still holds the last practice frame; the next step
                // (this tick's successor) shows the frame after A, continuing the rewind motion.
                resetFlashFilter()
                statusDirty = true
            } catch {
                practiceSlot = nil
                practiceLength = 0
                structureDirty = true
                notice(String(localized: "Point A of section \(slot + 1) can’t be found, so repeating stopped"))
            }
        }
    }

    /// "Practice This Section": enters practice at the slot's A and autoplays (never records).
    func startPractice(_ slot: Int) {
        guard let s = session else { return }
        if fastForward { endFastForward(s); uiFastForwardHeld = false }
        let wasMode = s.mode
        do { try s.practiceGotoA(slot) } catch {
            if let e = error as? RNError, e.status == RN_ERR_NOT_FOUND {
                notice(String(localized: "Point A of section \(slot + 1) hasn’t been set yet"))
            } else {
                reportError(String(localized: "Couldn’t start practicing"), error)
            }
            return
        }
        if wasMode != RN_MODE_PRACTICE { modeBeforePractice = wasMode }
        let info = s.practiceSlot(slot)
        practiceSlot = slot
        practiceLength = info.hasB ? info.length : 0
        practiceLoop.reset()
        practiceRewindStarted = false
        history.clear()
        stepRepeater.reset()
        advanceRemaining = 0
        endOfTake = false
        paused = false
        resetFlashFilter()
        publishVideo()
        structureDirty = true
        notice(info.hasB ? String(localized: "Practice: \(info.displayName) (returns to A at B and repeats; nothing is recorded)")
                         : String(localized: "Practice: \(info.displayName) (B isn’t set, so it doesn’t repeat; nothing is recorded)"))
    }

    /// "Stop Practicing": back to the take exactly where practice was entered (paused).
    func stopPractice() {
        guard let s = session, s.mode == RN_MODE_PRACTICE else { return }
        do { try s.setMode(modeBeforePractice) } catch { reportError(String(localized: "Couldn’t stop practicing"), error); return }
        practiceSlot = nil
        practiceLength = 0
        practiceLoop.reset()
        practiceRewindStarted = false
        history.release()
        paused = true
        advanceRemaining = 0
        audio.setMuted(true)
        publishVideo()
        structureDirty = true
        notice(String(localized: "Stopped practicing (the take is unchanged)"))
    }

    private func refreshPracticeLength(_ s: EngineSession, _ slot: Int) {
        guard slot == practiceSlot else { return }
        let info = s.practiceSlot(slot)
        practiceLength = info.hasA && info.hasB ? info.length : 0
        if !info.hasA { practiceSlot = nil }
    }

    func practiceSetA(_ slot: Int) {
        guard let s = session else { return }
        do {
            try s.practiceSetA(slot)
            if s.mode == RN_MODE_PRACTICE { practiceSlot = slot; practiceLoop.interrupt() }
            refreshPracticeLength(s, slot)
            structureDirty = true
            notice(String(localized: "Set A of section \(slot + 1). Keep playing and set B where it should end"))
        } catch { reportError(String(localized: "Couldn’t set A"), error) }
    }

    func practiceSetB(_ slot: Int) {
        guard let s = session else { return }
        do {
            try s.practiceSetB(slot)
            refreshPracticeLength(s, slot)
            structureDirty = true
            let len = s.practiceSlot(slot).length
            notice(String(localized: "Set B of section \(slot + 1) (length \(Engine.timecode(forFrame: len))). Use “Practice” to repeat it"))
        } catch let e as RNError {
            switch e.status {
            case RN_ERR_DISCONTINUITY:
                if practiceSetBFromTake(slot) { return } // A is on this take: B from take frames
                notice(String(localized: "Set B at a point reached by playing on from A (if you rewound, jumped or switched takes after A, start again from A or set A again)"))
            case RN_ERR_NOT_FOUND:
                notice(String(localized: "Set A of section \(slot + 1) first"))
            case RN_ERR_INVALID_ARG:
                notice(String(localized: "A and B are at the same position. Play a little further before setting B"))
            default:
                reportError(String(localized: "Couldn’t set B"), e)
            }
        } catch { reportError(String(localized: "Couldn’t set B"), error) }
    }

    func practiceRename(_ slot: Int, _ name: String) {
        guard let s = session else { return }
        do { try s.practiceRename(slot, name: name); structureDirty = true } catch { reportError(String(localized: "Couldn’t rename"), error) }
    }

    func practiceClear(_ slot: Int) {
        guard let s = session else { return }
        do {
            try s.practiceClear(slot)
            refreshPracticeLength(s, slot)
            structureDirty = true
        } catch { reportError(String(localized: "Couldn’t clear the section"), error) }
    }

    // MARK: stepping / pause (emulation thread)

    /// Paused D-pad (InputManager, routed away from the game): step now, repeat while held.
    func pausedStep(_ dir: Int, down: Bool) {
        guard let s = session else { return }
        if down {
            guard paused else { return }
            stepFrame(stepRepeater.press(dir), s)
        } else {
            stepRepeater.release(dir)
        }
    }

    /// One frame forward (+1) or back (-1), pausing first.
    func stepFrame(_ dir: Int, _ s: EngineSession) {
        paused = true
        if s.mode == RN_MODE_PRACTICE { practiceLoop.interrupt() }
        if dir > 0 { advanceRemaining += 1; return }
        stepBack(s, 1)
    }

    func stepBack(_ s: EngineSession, _ n: UInt64) {
        paused = true
        advanceRemaining = 0
        if s.mode == RN_MODE_PRACTICE {
            practiceLoop.interrupt()
            do {
                try s.rewind(n)
                audio.setMuted(true)
                publishVideo()
            } catch { reportError(String(localized: "Couldn’t go back"), error) }
            return
        }
        seekCommand(s.frame >= n ? s.frame - n : 0)
    }

    func togglePause() {
        advanceRemaining = 0
        guard let s = session else { paused.toggle(); return }
        if paused, RecordToggle.shouldRestartOnPlay(recording: s.mode == RN_MODE_RECORD, practicing: s.mode == RN_MODE_PRACTICE,
                                                     frame: s.frame, takeLength: s.takeLength) {
            seekCommand(0) // replay at the take end: play from the beginning
        }
        paused.toggle()
    }

    /// Emulates exactly one frame. Returns false if nothing was emulated (end of take / error).
    /// frameStep: a paused frame advance (rnf_transport_step: replays inside the take, records only
    /// at its end); otherwise live play (records in record mode, branching mid-take).
    @discardableResult
    func stepOnce(_ s: EngineSession, audible: Bool, publish: Bool = true, frameStep: Bool = false) -> Bool {
        let mode = s.mode
        let live = mode == RN_MODE_RECORD || mode == RN_MODE_PRACTICE
        let tSample = HostClock.now()
        lastSample = tSample
        var p1: UInt8 = 0, p2: UInt8 = 0
        if live {
            // Practice: rn_frame is frozen, so a separate monotonic clock drives tap latching / turbo.
            let clock = mode == RN_MODE_PRACTICE ? practiceSeq : s.frame
            if mode == RN_MODE_PRACTICE { practiceSeq &+= 1 }
            rn_input_sample_game(input.handle, clock, &p1, &p2)
        }
        let eventTime = input.consumeEventTime()
        let ev = live ? pendingEvents : 0
        pendingEvents = 0
        let before = s.frame
        let info: rn_step_info
        do {
            info = try frameStep ? s.transportStep(p1: p1, p2: p2, events: ev) : s.step(p1: p1, p2: p2, events: ev)
        } catch {
            paused = true
            advanceRemaining = 0
            reportError(String(localized: "Couldn’t advance the frame"), error)
            return false
        }
        if mode != RN_MODE_PRACTICE && info.end_of_take != 0 && info.frame == before {
            if !endOfTake { notice(String(localized: "End of the take (press “Record” to continue recording from here)")) }
            endOfTake = true
            paused = true
            advanceRemaining = 0
            statusDirty = true
            return false
        }
        endOfTake = false
        if info.branched != 0 {
            structureDirty = true
            notice(String(localized: "Recording a new take from here. The old continuation is kept: “Back to Previous Take” returns to it"))
        }
        let tEmu = HostClock.now()
        if let v = s.video {
            if mode == RN_MODE_PRACTICE { history.append(v) }
            if publish {
                show(v, meta: FrameMeta(frame: info.frame, inputEventTime: eventTime, sampleTime: tSample, emulatedTime: tEmu))
            }
        }
        if audible { audio.push(s.audio(), frameRate: emulationRate); latency.recordAudioRatio(audio.ratio) }
        if mode != RN_MODE_PRACTICE { pendingThumbnail = info.frame }
        lastStep = tEmu
        latency.recordStep(sampleToEmulated: tEmu - tSample)
        statusDirty = true
        return true
    }

    private func handleHotkeys(_ e: UInt32, _ s: EngineSession) {
        func on(_ bit: UInt32) -> Bool { e & bit != 0 }
        if on(RN_HK_PAUSE) { togglePause() }
        if on(RN_HK_FRAME_ADVANCE) { stepFrame(1, s) }
        if on(RN_HK_STEP_BACK) { stepFrame(-1, s) }
        if on(RN_HK_SLOW) { slow = slow.toggled; notice(slow == .normal ? String(localized: "Normal Speed") : String(localized: "Slow 1/2")) }
        if on(RN_HK_BOOKMARK) { addBookmark(name: nil) }
        if on(RN_HK_SOFT_RESET) { requestEvent(UInt8(RN_EV_SOFT_RESET)) }
        if on(RN_HK_POWER_CYCLE) { requestEvent(UInt8(RN_EV_POWER_CYCLE)) }
        if on(RN_HK_TOGGLE_MODE) { toggleRecord() }
        if on(RN_HK_SAVE) { save(nil) }
        if on(RN_HK_UNDO_TAKE) { undoTake() }
    }

    // MARK: operations (emulation thread)

    func requestEvent(_ ev: UInt8) {
        guard let s = session else { return }
        if s.mode == RN_MODE_REPLAY {
            notice(String(localized: "Reset is only available in record mode (or while practicing)"))
            return
        }
        pendingEvents |= ev
        if paused { advanceRemaining += 1 } // apply immediately so the user sees it
        let power = ev == UInt8(RN_EV_POWER_CYCLE)
        if s.mode == RN_MODE_PRACTICE {
            notice(power ? String(localized: "Power cycle (practicing: not recorded)") : String(localized: "Soft reset (practicing: not recorded)"))
        } else {
            notice(power ? String(localized: "Recording a power cycle") : String(localized: "Recording a soft reset"))
        }
    }

    func setRecording(_ rec: Bool) {
        guard let s = session else { return }
        if s.mode == RN_MODE_PRACTICE { stopPractice() }
        if fastForward { endFastForward(s) }
        do {
            try s.setMode(rec ? RN_MODE_RECORD : RN_MODE_REPLAY)
            endOfTake = false
        } catch { reportError(String(localized: "Couldn’t switch modes"), error) }
        statusDirty = true
    }

    /// The "Record" toggle button (see RecordToggle). In practice it leaves practice first.
    func toggleRecord() {
        guard let s = session else { return }
        if s.mode == RN_MODE_PRACTICE { stopPractice(); return }
        if fastForward { endFastForward(s) }
        let plan = RecordToggle.plan(recording: s.mode == RN_MODE_RECORD, frame: s.frame, takeLength: s.takeLength)
        if !plan.record && s.takeLength == 0 { notice(String(localized: "Nothing has been recorded yet")); return }
        setRecording(plan.record)
        if let f = plan.seek { seekCommand(f) }
        advanceRemaining = 0
        paused = !plan.play
        if plan.play { audio.setMuted(true) } // unmuted by the next audible step
        notice(plan.record ? String(localized: "Record mode: recording continues from here with your next input") : String(localized: "Playback mode: plays the recorded take (nothing is recorded)"))
    }

    /// "Re-record from Here": record from the current frame (branches if before the take end).
    func rerecordHere() {
        guard let s = session else { return }
        if s.mode == RN_MODE_PRACTICE { stopPractice() }
        setRecording(true)
        if s.mode == RN_MODE_RECORD { paused = false; slow = .normal }
    }

    func addBookmark(name: String?) {
        guard let s = session else { return }
        if s.mode == RN_MODE_PRACTICE { notice(Self.practiceBlockedText); return }
        let n = name ?? String(localized: "Bookmark \(s.bookmarks().count + 1) (\(Engine.timecode(forFrame: s.frame)))")
        do { try s.addBookmark(name: n); structureDirty = true } catch { reportError(String(localized: "Couldn’t add the bookmark"), error) }
    }

    func undoTake() {
        guard let s = session else { return }
        if s.mode == RN_MODE_PRACTICE { notice(Self.practiceBlockedText); return }
        if s.undoDepth == 0 { notice(String(localized: "There is no previous take to go back to")); return }
        do {
            try s.undoTakeSwitch()
            paused = true
            endOfTake = false
            audio.setMuted(true)
            publishVideo()
            structureDirty = true
            notice(String(localized: "Went back to the previous take"))
        } catch { reportError(String(localized: "Couldn’t go back to the previous take"), error) }
    }

    func save(_ done: ((Bool) -> Void)?) {
        guard let s = session else { done.map { f in onMain { f(false) } }; return }
        if s.projectDir.isEmpty {
            onMain { done?(false) }
            notice(String(localized: "This session has no save location yet. Use “Save As…”"))
            return
        }
        do {
            try s.save()
            autosaveFailed = false
            statusDirty = true
            onMain { done?(true) }
        } catch {
            reportError(String(localized: "Couldn’t save (your work is still kept in memory)"), error)
            onMain { done?(false) }
        }
    }

    /// Autosave = engine journal append + fsync. The session is single-threaded, so it runs
    /// on this thread, but only right after a frame was published and only when the next
    /// frame deadline is far enough away; its cost is shown in the latency overlay.
    private func maybeAutosave(_ s: EngineSession, slack: Double) {
        let now = HostClock.now()
        guard autosaveInterval > 0, !s.projectDir.isEmpty,
              autosaveSoon || HostClock.seconds(now - lastAutosave) >= autosaveInterval else { return }
        if slack < 0.008 && !paused { return } // try again next tick
        lastAutosave = now
        autosaveSoon = false
        guard s.hasUnsavedChanges else { return }
        do {
            if tempSession && paused && (lastFullSave == 0 || HostClock.seconds(now - lastFullSave) >= 30) {
                try s.save()
                lastFullSave = now
            } else {
                try s.autosave()
            }
            autosaveFailed = false
        } catch {
            if !autosaveFailed { reportError(String(localized: "Autosave failed (your work is still kept in memory)"), error) }
            autosaveFailed = true
        }
        latency.recordAutosave(ticks: HostClock.now() - now)
    }

    /// Persists now (quit, app in background): a full save for the temporary project, the autosave
    /// journal for a normal project (its last full save stays what "Don’t Save" returns to).
    /// Returns an error message, nil on success.
    func flushForResume(fullSave: Bool) -> String? {
        guard let s = session, !s.projectDir.isEmpty, s.hasUnsavedChanges else { return nil }
        do {
            if fullSave { try s.save(); lastFullSave = HostClock.now() } else { try s.autosave() }
            lastAutosave = HostClock.now()
            autosaveFailed = false
            statusDirty = true
            return nil
        } catch {
            return (error as? LocalizedError)?.errorDescription ?? "\(error)"
        }
    }

    /// After install(): back to the recorded take mode / position, paused.
    func applyResume(_ r: ResumeRecord) {
        guard let s = session else { return }
        do { try SessionResume.apply(r, to: s) } catch { reportError(String(localized: "Couldn’t move to the previous position"), error) }
        paused = true
        audio.setMuted(true)
        endOfTake = false
        markStructureDirty()
        publishVideo()
    }

    // MARK: publishing

    private var lastPublishedTake: UInt64 = .max

    private func publishStatus(force: Bool) {
        guard force || statusDirty || structureDirty else { return }
        if let s = session {
            // Take lengths / new takes change while recording: refresh the lists ~1/s.
            if s.activeTake != lastPublishedTake || tickCount % 60 == 0 { structureDirty = true }
            syncThumbnailTake(s)
            lastPublishedTake = s.activeTake
        }
        var st = EmuStatus()
        if let s = session {
            st.hasSession = true
            st.frame = s.frame
            st.takeLength = s.takeLength
            st.recording = ff.showsRecording(s)
            st.paused = paused
            st.slow = slow
            st.rewinding = rewinding
            st.fastForward = fastForward && ff.active && !ffBlocked
            st.endOfTake = endOfTake
            st.activeTake = s.activeTake
            st.undoDepth = s.undoDepth
            st.unsaved = s.hasUnsavedChanges
            st.projectPath = s.projectDir
            st.romPath = s.romPath
            st.advancePending = advanceRemaining
            st.flashActive = flashActiveShown
            if s.mode == RN_MODE_PRACTICE {
                st.practicing = true
                st.practiceSlot = practiceSlot ?? -1
                st.practiceFrame = s.practiceFrame
                st.practiceLength = practiceSlot != nil ? practiceLength : 0
                st.practiceLooping = practiceLoop.isLooping
                st.practiceLoops = practiceLoop.loops
            }
        }
        statusDirty = false
        var structure: SessionStructure?
        if structureDirty {
            structureDirty = false
            structure = session.map { SessionStructure(bookmarks: $0.bookmarks(), takes: $0.takes(), practiceSlots: $0.practiceSlots()) }
                ?? SessionStructure()
        }
        if let structure { cachedTakeCount = structure.takes.count }
        st.takeCount = session == nil ? 0 : cachedTakeCount
        onMain { [weak self] in
            guard let self else { return }
            if let structure { self.onStructure?(structure) }
            self.onStatus?(st)
        }
    }
}
