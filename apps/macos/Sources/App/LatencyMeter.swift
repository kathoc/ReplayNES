// Rolling latency / pacing statistics (latency overlay, snapshot JSON, --stats-log) and the
// optional per-frame log (--frame-log, analysed by scripts/perf-smoke.sh).
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Thread-safe. Writers: emulation thread, Metal presented handlers; readers: main thread.
final class LatencyMeter {
    struct Snapshot {
        var sampleToEmulatedMs = 0.0
        var emulatedToPresentMs = 0.0
        var sampleToPresentMs = 0.0  // input sample -> frame on screen (presentedTime), EMA
        var inputToPresentMs = 0.0   // only frames that consumed a fresh input change
        var lastInputToPresentMs = 0.0
        var stepMs = 0.0
        var lastAutosaveMs = 0.0
        var presentedFPS = 0.0
        var audioFillMs = 0.0
        var audioUnderruns: UInt64 = 0
        var audioDropped: UInt64 = 0
        var audioCallbackFrames: UInt32 = 0
        var audioOutputLatencyMs = 0.0
        var audioRatio = 1.0         // dynamic rate control (output / input samples)
        var lateTicks: UInt64 = 0
        var displayGPUMs = 0.0       // GPU time of the display command buffer for a new frame (EMA)
        var displayGPUMaxMs = 0.0    // max over the last second
        var crtInfo = ""             // "" = CRT off; else tube size / signal path
        // Pacing (FramePacing.swift). Counters are cumulative; *Max* are over the last second.
        var pacing = ""              // "display link 120 Hz / 2"
        var refreshHz = 0.0
        var emulatedFPS = 0.0        // frames emulated per second
        var presentCount: UInt64 = 0
        var presentHitches: UInt64 = 0     // new frame shown > 1.5 frame periods after the previous
        var offCadence: UInt64 = 0         // interval differs from the expected one by > 1/2 refresh
        var skippedFrames: UInt64 = 0      // emulated frames never shown
        var presentIntervalMaxMs = 0.0
        var missedRefreshes: UInt64 = 0    // frames presented after the refresh they were made for
        var droppedFrames: UInt64 = 0      // new frames the compositor never showed (a repeat did)
        var foreignCallbacks: UInt64 = 0   // display-link updates delivered on another thread (skipped)
        var repeatPresents: UInt64 = 0     // the same picture presented again (keeps the cadence)
        var backlogDrains: UInt64 = 0      // repeat presents skipped because the queue was behind
        var inputLeadMs = 0.0              // just-in-time input sample before the commit deadline
        var drawCount: UInt64 = 0          // frames rendered during play
        var drawLate: UInt64 = 0           // rendered > 1.5 frame periods after the previous one
        var drawGapMaxMs = 0.0
        var tickWakeLate: UInt64 = 0       // emulation ticks that started > 4 ms after their deadline
        var tickWakeMaxMs = 0.0
        var emulationCPU = 0.0             // CPU seconds of the emulation thread (cumulative)
        var layerPixels = ""               // game layer drawable size (full screen: should equal the screen)
    }

    private let lock = NSLock()
    private var s = Snapshot()
    private var presentCount = 0
    private var fpsWindowStart = HostClock.now()
    private var gpuMaxWindow = 0.0, gpuWindowStart = HostClock.now()
    private var pacing = FramePacing()
    private var draws = CallbackRegularity(lateAfter: 1.5 / 120)
    private var stepCount = 0, stepWindowStart = HostClock.now()
    private var tickWakeWindowMax = 0.0
    private var pacingWindowStart = HostClock.now()
    private var expectedInterval = FramePacing.period
    private var refreshInterval = 1.0 / 120
    private var frameLog: [String]?

    private static func ema(_ old: Double, _ v: Double) -> Double { old == 0 ? v : old * 0.9 + v * 0.1 }

    // MARK: emulation thread

    func recordStep(sampleToEmulated: UInt64) {
        let now = HostClock.now()
        lock.lock(); s.sampleToEmulatedMs = Self.ema(s.sampleToEmulatedMs, HostClock.seconds(sampleToEmulated) * 1000)
        s.stepMs = s.sampleToEmulatedMs
        stepCount += 1
        let dt = HostClock.seconds(now - stepWindowStart)
        if dt >= 1 { s.emulatedFPS = Double(stepCount) / dt; stepCount = 0; stepWindowStart = now }
        lock.unlock()
    }

    /// How late the tick woke up relative to its deadline.
    func recordTickWake(lateTicks: UInt64) {
        let ms = HostClock.seconds(lateTicks) * 1000
        lock.lock()
        if ms > 4 { s.tickWakeLate &+= 1 }
        tickWakeWindowMax = max(tickWakeWindowMax, ms)
        lock.unlock()
    }

    /// Display cadence in use: what a steady frame interval is, for judder counting.
    func recordCadence(pacing: String, refresh: Double, expectedInterval: Double, inputLead: Double) {
        lock.lock()
        s.pacing = pacing
        s.refreshHz = refresh > 0 ? 1 / refresh : 0
        if refresh > 0 { refreshInterval = refresh }
        self.expectedInterval = expectedInterval
        s.inputLeadMs = inputLead * 1000
        lock.unlock()
    }

    func recordThreadCPU(emulation: Double) { lock.lock(); s.emulationCPU = emulation; lock.unlock() }

    func recordAudioRatio(_ r: Double) { lock.lock(); s.audioRatio = r; lock.unlock() }
    func recordRepeat() { lock.lock(); s.repeatPresents &+= 1; lock.unlock() }
    func recordDropped() { lock.lock(); s.droppedFrames &+= 1; lock.unlock() }
    func recordLayerSize(_ size: CGSize) { lock.lock(); s.layerPixels = "\(Int(size.width))x\(Int(size.height))"; lock.unlock() }
    func recordForeignCallback() { lock.lock(); s.foreignCallbacks &+= 1; lock.unlock() }
    func recordBacklogDrain() { lock.lock(); s.backlogDrains &+= 1; lock.unlock() }

    /// A frame was rendered. `refresh` = the expected interval in seconds.
    func recordDraw(refresh: Double) {
        let t = HostClock.seconds(HostClock.now())
        lock.lock()
        draws.lateAfter = refresh * 1.5
        draws.tick(at: t)
        s.drawCount = draws.count
        s.drawLate = draws.late
        rollPacingWindow()
        lock.unlock()
    }

    /// Once a second: publishes the window maxima (lock held).
    private func rollPacingWindow() {
        let now = HostClock.now()
        guard HostClock.seconds(now - pacingWindowStart) >= 1 else { return }
        pacingWindowStart = now
        s.presentIntervalMaxMs = pacing.takeWindowMax() * 1000
        s.drawGapMaxMs = draws.takeWindowMax() * 1000
        s.tickWakeMaxMs = tickWakeWindowMax
        tickWakeWindowMax = 0
    }

    // MARK: presented handlers (any thread)

    /// A newly emulated frame reached the screen. `commit`: mach ticks when its command buffer was
    /// committed. Returns true when it missed the refresh it was made for (display-link pacing).
    @discardableResult
    func recordPresent(meta: FrameMeta, commit: UInt64, presentedSeconds: Double, gpu: (start: Double, end: Double) = (0, 0)) -> Bool {
        let emu = HostClock.seconds(meta.emulatedTime)
        lock.lock()
        defer { lock.unlock() }
        if presentedSeconds > emu {
            s.emulatedToPresentMs = Self.ema(s.emulatedToPresentMs, (presentedSeconds - emu) * 1000)
        }
        if meta.sampleTime != 0 {
            let v = (presentedSeconds - HostClock.seconds(meta.sampleTime)) * 1000
            if v > 0 { s.sampleToPresentMs = Self.ema(s.sampleToPresentMs, v) }
        }
        if meta.inputEventTime != 0 {
            let ev = HostClock.seconds(meta.inputEventTime)
            if presentedSeconds > ev {
                let v = (presentedSeconds - ev) * 1000
                s.lastInputToPresentMs = v
                s.inputToPresentMs = Self.ema(s.inputToPresentMs, v)
            }
        }
        // Missed = shown at least a refresh after the one it was made for. (Full screen on a
        // ProMotion panel, presentation times can sit a fraction of a refresh off the display
        // link's targets while every frame still keeps its slot.)
        let missed = meta.targetPresentation > 0 && presentedSeconds > meta.targetPresentation + refreshInterval * 0.75
        if missed { s.missedRefreshes &+= 1 }
        if let l = pacing.lastPresent, meta.frame == l.frame + 1, presentedSeconds > l.time,
           presentedSeconds - l.time < FramePacing.continuityLimit,
           abs(presentedSeconds - l.time - expectedInterval) > refreshInterval / 2 {
            s.offCadence &+= 1
        }
        pacing.present(frame: meta.frame, at: presentedSeconds)
        s.presentCount = pacing.presents
        s.presentHitches = pacing.hitches
        s.skippedFrames = pacing.skipped
        rollPacingWindow()
        presentCount += 1
        let now = HostClock.now()
        let dt = HostClock.seconds(now - fpsWindowStart)
        if dt >= 1 { s.presentedFPS = Double(presentCount) / dt; presentCount = 0; fpsWindowStart = now }
        if frameLog != nil {
            func sec(_ t: UInt64) -> String { t == 0 ? "0" : String(format: "%.6f", HostClock.seconds(t)) }
            frameLog?.append("\(meta.frame),\(sec(meta.tickStart)),\(sec(meta.sampleTime)),\(sec(meta.emulatedTime)),\(sec(commit)),"
                + String(format: "%.6f,%.6f,", meta.targetPresentation, presentedSeconds) + "\(sec(meta.inputEventTime)),0," + String(format: "%.6f,%.6f", gpu.start, gpu.end))
        }
        return missed
    }

    func recordDisplayGPU(ms: Double, crtInfo: String) {
        lock.lock(); defer { lock.unlock() }
        s.displayGPUMs = Self.ema(s.displayGPUMs, ms)
        s.crtInfo = crtInfo
        gpuMaxWindow = max(gpuMaxWindow, ms)
        let now = HostClock.now()
        if HostClock.seconds(now - gpuWindowStart) >= 1 { s.displayGPUMaxMs = gpuMaxWindow; gpuMaxWindow = 0; gpuWindowStart = now }
    }

    func recordAutosave(ticks: UInt64) { lock.lock(); s.lastAutosaveMs = HostClock.seconds(ticks) * 1000; lock.unlock() }
    func recordLateTick() { lock.lock(); s.lateTicks += 1; lock.unlock() }

    // MARK: frame log (--frame-log)

    /// kind 0: a newly emulated frame; 1: the same picture presented again (repeat refresh).
    /// gpuStart / gpuEnd: GPU execution of the new frame's command buffers (0 for repeats).
    static let frameLogHeader = "frame,tick,sample,emulated,commit,target,presented,event,kind,gpuStart,gpuEnd"

    /// A repeat present of the picture of `frame` reached the screen (frame log only).
    func recordRepeatPresented(frame: UInt64, target: Double, presentedSeconds: Double) {
        lock.lock(); defer { lock.unlock() }
        guard frameLog != nil else { return }
        frameLog?.append("\(frame),0,0,0,0," + String(format: "%.6f,%.6f,", target, presentedSeconds) + "0,1,0,0")
    }

    func startFrameLog() { lock.lock(); if frameLog == nil { frameLog = [] }; lock.unlock() }

    /// Rows logged since the last call (CSV lines, no header).
    func takeFrameLog() -> [String] {
        lock.lock(); defer { lock.unlock() }
        guard let rows = frameLog else { return [] }
        frameLog = []
        return rows
    }

    // MARK: readers

    func snapshot(audio: AudioOutput?) -> Snapshot {
        lock.lock()
        var out = s
        lock.unlock()
        if let audio {
            let st = audio.stats()
            out.audioFillMs = Double(st.fill) / 48.0
            out.audioUnderruns = st.underruns
            out.audioDropped = st.dropped_samples
            out.audioCallbackFrames = st.last_request
            out.audioOutputLatencyMs = audio.outputLatencySeconds * 1000
        }
        return out
    }
}
