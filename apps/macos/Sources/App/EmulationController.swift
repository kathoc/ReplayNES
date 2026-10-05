// Emulation thread: the only owner of the rn_session.
// The host clock decides only WHEN rn_step is called (pacing, pause, slow, advance, rewind);
// WHAT is emulated is fully determined by the recorded input/event stream.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum SlowRate: Int, CaseIterable, Identifiable {
    case normal = 1, half = 2, quarter = 4
    var id: Int { rawValue }
    var label: String { self == .normal ? "等速" : self == .half ? "1/2" : "1/4" }
    var next: SlowRate { self == .normal ? .half : self == .half ? .quarter : .normal }
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
}

final class EmulationController {
    let frames = FrameBuffer()
    let audio = AudioOutput()
    let latency = LatencyMeter()
    let input: InputManager

    // Callbacks, always invoked on the main thread.
    var onStatus: ((EmuStatus) -> Void)?
    var onStructure: (([BookmarkInfo], [TakeInfo]) -> Void)?
    var onError: ((String, String) -> Void)?   // title, message
    var onNotice: ((String) -> Void)?

    private var thread: Thread?
    private let cmdLock = NSLock()
    private var commands: [(EmulationController) -> Void] = []
    private var running = true

    // ---- emulation-thread state (never touched from other threads) ----
    private(set) var session: EngineSession?
    var paused = true { didSet { if paused != oldValue { statusDirty = true } } }
    var slow: SlowRate = .normal { didSet { statusDirty = true } }
    var advanceRemaining = 0
    var uiRewindHeld = false
    var uiFastForwardHeld = false
    var pendingEvents: UInt8 = 0
    var scrubTarget: UInt64?
    var pauseAfterRewind = true
    var autosaveInterval: Double = 5
    private var rewinding = false
    private var fastForward = false
    private var rewindTicks = 0
    private var tickCount: UInt64 = 0
    private var statusDirty = true
    private var structureDirty = true
    private var endOfTake = false
    private var cachedTakeCount = 0
    private var lastAutosave: UInt64 = 0
    private var autosaveFailed = false
    private var nextDeadline: UInt64 = 0
    private let period: UInt64 = HostClock.ticks(seconds: Double(RN_FPS_DEN) / Double(RN_FPS_NUM))

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

    func shutdown() {
        _ = sync(timeout: 10) { emu in emu.running = false }
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
        paused = true
        slow = .normal
        advanceRemaining = 0
        scrubTarget = nil
        pendingEvents = 0
        endOfTake = false
        audio.setMuted(true)
        lastAutosave = HostClock.now()
        autosaveFailed = false
        structureDirty = true
        statusDirty = true
        if let s, let v = s.video { frames.publish(v, meta: FrameMeta(frame: s.frame)) } else { frames.clear() }
        publishStatus(force: true)
    }

    func markStructureDirty() { structureDirty = true; statusDirty = true }

    func publishVideo() {
        guard let s = session, let v = s.video else { return }
        frames.publish(v, meta: FrameMeta(frame: s.frame, emulatedTime: HostClock.now()))
        statusDirty = true
    }

    /// Seek helper used by commands: pauses, mutes, refreshes the picture.
    func seekCommand(_ f: UInt64) {
        guard let s = session else { return }
        do {
            try s.seek(min(f, s.takeLength))
            endOfTake = false
        } catch { reportError("移動できませんでした", error) }
        paused = true
        audio.setMuted(true)
        publishVideo()
    }

    // MARK: thread loop

    private func threadMain() {
        nextDeadline = HostClock.now()
        while running {
            let now = HostClock.now()
            if now < nextDeadline { mach_wait_until(nextDeadline) }
            tick()
            nextDeadline &+= period
            let after = HostClock.now()
            if after > nextDeadline &+ period * 6 {
                // Stalled (app nap, debugger, heavy seek): resync. Never "catch up" by stepping
                // extra frames for wall-clock reasons.
                latency.recordLateTick()
                nextDeadline = after
            }
        }
        // Thread exit: release the session on its own thread.
        session = nil
    }

    private func runCommands() {
        cmdLock.lock()
        let cmds = commands
        commands.removeAll(keepingCapacity: true)
        cmdLock.unlock()
        for c in cmds { c(self) }
    }

    private func tick() {
        tickCount &+= 1
        runCommands()
        guard let s = session else {
            publishStatus(force: false)
            return
        }

        var edges: UInt32 = 0, held: UInt32 = 0
        rn_input_poll_hotkeys(input.handle, &edges, &held)
        if edges != 0 { handleHotkeys(edges, s) }

        let wantRewind = uiRewindHeld || held & UInt32(RN_HK_REWIND) != 0
        let wantFF = uiFastForwardHeld || held & UInt32(RN_HK_FAST_FORWARD) != 0
        if wantRewind != rewinding || wantFF != fastForward { statusDirty = true }
        fastForward = wantFF

        if wantRewind {
            rewinding = true
            audio.setMuted(true)
            rewindTicks += 1
            let n: UInt64 = rewindTicks > 240 ? 4 : rewindTicks > 90 ? 2 : 1
            if s.frame > 0 {
                do {
                    try s.rewind(n)
                    endOfTake = false
                    publishVideo()
                } catch { reportError("巻き戻しに失敗しました", error); uiRewindHeld = false }
            }
            finishTick(s)
            return
        }
        if rewinding {
            rewinding = false
            rewindTicks = 0
            if pauseAfterRewind { paused = true }
        }

        if let t = scrubTarget {
            scrubTarget = nil
            seekCommand(t)
            finishTick(s)
            return
        }

        var steps = 0
        var audible = false
        if paused {
            if advanceRemaining > 0 { steps = 1; advanceRemaining -= 1; statusDirty = true }
        } else if fastForward {
            steps = 4
        } else {
            if tickCount % UInt64(slow.rawValue) == 0 { steps = 1 }
            audible = slow == .normal
        }
        audio.setMuted(!audible)
        for _ in 0..<steps {
            if !stepOnce(s, audible: audible) { break }
        }
        maybeAutosave(s)
        finishTick(s)
    }

    private func finishTick(_ s: EngineSession) {
        if tickCount % 3 == 0 || structureDirty { publishStatus(force: false) }
    }

    /// Emulates exactly one frame. Returns false if nothing was emulated (end of take / error).
    @discardableResult
    func stepOnce(_ s: EngineSession, audible: Bool) -> Bool {
        let recording = s.mode == RN_MODE_RECORD
        let tSample = HostClock.now()
        var p1: UInt8 = 0, p2: UInt8 = 0
        if recording { rn_input_sample_game(input.handle, s.frame, &p1, &p2) }
        let eventTime = input.consumeEventTime()
        let ev = recording ? pendingEvents : 0
        pendingEvents = 0
        let before = s.frame
        let info: rn_step_info
        do {
            info = try s.step(p1: p1, p2: p2, events: ev)
        } catch {
            paused = true
            advanceRemaining = 0
            reportError("フレームを進められませんでした", error)
            return false
        }
        if info.end_of_take != 0 && info.frame == before {
            if !endOfTake { notice("テイクの終端です。続きを録るには「ここから録り直す」") }
            endOfTake = true
            paused = true
            advanceRemaining = 0
            statusDirty = true
            return false
        }
        endOfTake = false
        if info.branched != 0 {
            structureDirty = true
            notice("新しいテイクを作成しました。以前の続きは「前の試行へ戻す」で戻せます")
        }
        let tEmu = HostClock.now()
        if let v = s.video {
            frames.publish(v, meta: FrameMeta(frame: info.frame, inputEventTime: eventTime, sampleTime: tSample, emulatedTime: tEmu))
        }
        if audible { audio.push(s.audio()) }
        latency.recordStep(sampleToEmulated: tEmu - tSample)
        statusDirty = true
        return true
    }

    private func handleHotkeys(_ e: UInt32, _ s: EngineSession) {
        func on(_ bit: UInt32) -> Bool { e & bit != 0 }
        if on(RN_HK_PAUSE) { paused.toggle(); advanceRemaining = 0 }
        if on(RN_HK_FRAME_ADVANCE) { paused = true; advanceRemaining += 1 }
        if on(RN_HK_STEP_BACK) {
            paused = true
            if s.frame > 0 { seekCommand(s.frame - 1) }
        }
        if on(RN_HK_SLOW) { slow = slow.next }
        if on(RN_HK_BOOKMARK) { addBookmark(name: nil) }
        if on(RN_HK_SOFT_RESET) { requestEvent(UInt8(RN_EV_SOFT_RESET)) }
        if on(RN_HK_POWER_CYCLE) { requestEvent(UInt8(RN_EV_POWER_CYCLE)) }
        if on(RN_HK_TOGGLE_MODE) { setRecording(s.mode != RN_MODE_RECORD) }
        if on(RN_HK_SAVE) { save(nil) }
        if on(RN_HK_UNDO_TAKE) { undoTake() }
    }

    // MARK: operations (emulation thread)

    func requestEvent(_ ev: UInt8) {
        guard let s = session else { return }
        if s.mode != RN_MODE_RECORD {
            notice("リセットは録画モードでのみ記録できます")
            return
        }
        pendingEvents |= ev
        if paused { advanceRemaining += 1 } // apply immediately so the user sees it
        notice(ev == UInt8(RN_EV_POWER_CYCLE) ? "電源再投入を記録します" : "ソフトリセットを記録します")
    }

    func setRecording(_ rec: Bool) {
        guard let s = session else { return }
        do {
            try s.setMode(rec ? RN_MODE_RECORD : RN_MODE_REPLAY)
            endOfTake = false
        } catch { reportError("モードを切り替えられませんでした", error) }
        statusDirty = true
    }

    /// 「ここから録り直す」: record from the current frame (branches if before the take end).
    func rerecordHere() {
        guard let s = session else { return }
        setRecording(true)
        if s.mode == RN_MODE_RECORD { paused = false; slow = .normal }
    }

    func addBookmark(name: String?) {
        guard let s = session else { return }
        let n = name ?? "ブックマーク \(s.bookmarks().count + 1) (\(Engine.timecode(forFrame: s.frame)))"
        do { try s.addBookmark(name: n); structureDirty = true } catch { reportError("ブックマークを追加できませんでした", error) }
    }

    func undoTake() {
        guard let s = session else { return }
        if s.undoDepth == 0 { notice("戻せる試行はありません"); return }
        do {
            try s.undoTakeSwitch()
            paused = true
            endOfTake = false
            audio.setMuted(true)
            publishVideo()
            structureDirty = true
            notice("前の試行へ戻しました")
        } catch { reportError("前の試行へ戻せませんでした", error) }
    }

    func save(_ done: ((Bool) -> Void)?) {
        guard let s = session else { done.map { f in onMain { f(false) } }; return }
        if s.projectDir.isEmpty {
            onMain { done?(false) }
            notice("このセッションはまだ保存先がありません。「別名で保存」を使ってください")
            return
        }
        do {
            try s.save()
            autosaveFailed = false
            statusDirty = true
            onMain { done?(true) }
        } catch {
            reportError("保存できませんでした（作業内容はメモリ上に保持されています）", error)
            onMain { done?(false) }
        }
    }

    /// Autosave = engine journal append + fsync. The session is single-threaded, so it runs
    /// on this thread, but only right after a frame was published and only when the next
    /// frame deadline is far enough away; its cost is shown in the latency overlay.
    private func maybeAutosave(_ s: EngineSession) {
        let now = HostClock.now()
        guard autosaveInterval > 0, !s.projectDir.isEmpty,
              HostClock.seconds(now - lastAutosave) >= autosaveInterval else { return }
        let slack = nextDeadline &+ period > now ? HostClock.seconds(nextDeadline &+ period - now) : 0
        if slack < 0.008 && !paused { return } // try again next tick
        lastAutosave = now
        guard s.hasUnsavedChanges else { return }
        do {
            try s.autosave()
            autosaveFailed = false
        } catch {
            if !autosaveFailed { reportError("自動保存に失敗しました（作業内容はメモリ上に保持されています）", error) }
            autosaveFailed = true
        }
        latency.recordAutosave(ticks: HostClock.now() - now)
    }

    // MARK: publishing

    private var lastPublishedTake: UInt64 = .max

    private func publishStatus(force: Bool) {
        guard force || statusDirty || structureDirty else { return }
        if let s = session {
            // Take lengths / new takes change while recording: refresh the lists ~1/s.
            if s.activeTake != lastPublishedTake || tickCount % 60 == 0 { structureDirty = true }
            lastPublishedTake = s.activeTake
        }
        var st = EmuStatus()
        if let s = session {
            st.hasSession = true
            st.frame = s.frame
            st.takeLength = s.takeLength
            st.recording = s.mode == RN_MODE_RECORD
            st.paused = paused
            st.slow = slow
            st.rewinding = rewinding
            st.fastForward = fastForward && !paused
            st.endOfTake = endOfTake
            st.activeTake = s.activeTake
            st.undoDepth = s.undoDepth
            st.unsaved = s.hasUnsavedChanges
            st.projectPath = s.projectDir
            st.romPath = s.romPath
            st.advancePending = advanceRemaining
        }
        statusDirty = false
        var structure: ([BookmarkInfo], [TakeInfo])?
        if structureDirty {
            structureDirty = false
            structure = session.map { ($0.bookmarks(), $0.takes()) } ?? ([], [])
        }
        if let structure { cachedTakeCount = structure.1.count }
        st.takeCount = session == nil ? 0 : cachedTakeCount
        onMain { [weak self] in
            guard let self else { return }
            if let structure { self.onStructure?(structure.0, structure.1) }
            self.onStatus?(st)
        }
    }
}
