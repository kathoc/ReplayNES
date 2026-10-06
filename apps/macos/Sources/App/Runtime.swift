// Frame hand-off, real-time audio output and latency instrumentation.
// SPDX-License-Identifier: GPL-2.0-or-later
import AVFoundation
import AudioToolbox
import Foundation

enum HostClock {
    static let timebase: mach_timebase_info_data_t = {
        var tb = mach_timebase_info_data_t()
        mach_timebase_info(&tb)
        return tb
    }()
    @inline(__always) static func now() -> UInt64 { mach_absolute_time() }
    static func seconds(_ ticks: UInt64) -> Double { Double(ticks) * Double(timebase.numer) / Double(timebase.denom) / 1e9 }
    static func ticks(seconds s: Double) -> UInt64 { UInt64(s * 1e9 * Double(timebase.denom) / Double(timebase.numer)) }
}

/// Timestamps (mach ticks) attached to a published frame for latency measurement.
struct FrameMeta {
    var frame: UInt64 = 0
    var inputEventTime: UInt64 = 0  // last physical input change consumed by this frame (0 = none)
    var sampleTime: UInt64 = 0      // rn_input_sample_game
    var emulatedTime: UInt64 = 0    // rn_step returned
    // CRT signal side channel (display only; see rn_video_indices).
    var hasCodes = false            // raw PPU codes published with this frame
    var burstPhase: UInt32 = 0      // core colour-burst phase of the frame (0..2)
    var signalFrame: UInt64 = 0     // machine frame ordinal (CRT persistence / RF noise key)
    var flashAltered = false        // the flash filter changed this picture (CRT then uses the RGB path)
}

/// Latest-frame mailbox between the emulation thread (writer) and the Metal view (reader).
final class FrameBuffer {
    private let lock = NSLock()
    private var pixels = [UInt32](repeating: 0xFF00_0000, count: Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT))
    private var codes = [UInt16](repeating: 0x0F, count: Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT))
    private var meta = FrameMeta()
    private var seq: UInt64 = 0

    /// `codes`: raw PPU codes of the same picture (CRT signal path), copied when given.
    func publish(_ src: UnsafePointer<UInt32>, meta m: FrameMeta, codes c: UnsafePointer<UInt16>? = nil) {
        lock.lock()
        pixels.withUnsafeMutableBufferPointer { $0.baseAddress!.update(from: src, count: $0.count) }
        var m = m
        if let c { codes.withUnsafeMutableBufferPointer { $0.baseAddress!.update(from: c, count: $0.count) } } else { m.hasCodes = false }
        meta = m
        seq &+= 1
        lock.unlock()
    }

    func clear() {
        lock.lock()
        for i in pixels.indices { pixels[i] = 0xFF00_0000 }
        meta = FrameMeta()
        seq &+= 1
        lock.unlock()
    }

    /// Calls body only if a frame newer than `seen` exists. Returns the new sequence number.
    func readIfNewer(than seen: UInt64, _ body: (UnsafePointer<UInt32>, FrameMeta) -> Void) -> UInt64 {
        lock.lock()
        defer { lock.unlock() }
        if seq == seen { return seen }
        pixels.withUnsafeBufferPointer { body($0.baseAddress!, meta) }
        return seq
    }

    /// Like readIfNewer, also handing out the raw PPU codes (nil when the frame has none).
    func readFrameIfNewer(than seen: UInt64, _ body: (UnsafePointer<UInt32>, UnsafePointer<UInt16>?, FrameMeta) -> Void) -> UInt64 {
        lock.lock()
        defer { lock.unlock() }
        if seq == seen { return seen }
        pixels.withUnsafeBufferPointer { px in
            codes.withUnsafeBufferPointer { cd in body(px.baseAddress!, meta.hasCodes ? cd.baseAddress! : nil, meta) }
        }
        return seq
    }
}

/// Rolling latency / health statistics. Thread-safe.
final class LatencyMeter {
    struct Snapshot {
        var sampleToEmulatedMs = 0.0
        var emulatedToPresentMs = 0.0
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
        var lateTicks: UInt64 = 0
        var displayGPUMs = 0.0       // GPU time of the display command buffer for a new frame (EMA)
        var displayGPUMaxMs = 0.0    // max over the last second
        var crtInfo = ""             // "" = CRT off; else tube size / signal path
    }

    private let lock = NSLock()
    private var s = Snapshot()
    private var presentCount = 0
    private var fpsWindowStart = HostClock.now()
    private var gpuMaxWindow = 0.0, gpuWindowStart = HostClock.now()

    private static func ema(_ old: Double, _ v: Double) -> Double { old == 0 ? v : old * 0.9 + v * 0.1 }

    func recordStep(sampleToEmulated: UInt64) {
        lock.lock(); s.sampleToEmulatedMs = Self.ema(s.sampleToEmulatedMs, HostClock.seconds(sampleToEmulated) * 1000)
        s.stepMs = s.sampleToEmulatedMs; lock.unlock()
    }

    func recordPresent(meta: FrameMeta, presentedSeconds: Double) {
        let emu = HostClock.seconds(meta.emulatedTime)
        lock.lock()
        defer { lock.unlock() }
        if presentedSeconds > emu {
            s.emulatedToPresentMs = Self.ema(s.emulatedToPresentMs, (presentedSeconds - emu) * 1000)
        }
        if meta.inputEventTime != 0 {
            let ev = HostClock.seconds(meta.inputEventTime)
            if presentedSeconds > ev {
                let v = (presentedSeconds - ev) * 1000
                s.lastInputToPresentMs = v
                s.inputToPresentMs = Self.ema(s.inputToPresentMs, v)
            }
        }
        presentCount += 1
        let now = HostClock.now()
        let dt = HostClock.seconds(now - fpsWindowStart)
        if dt >= 1 { s.presentedFPS = Double(presentCount) / dt; presentCount = 0; fpsWindowStart = now }
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

/// AVAudioEngine output fed from a lock-free SPSC ring (producer: emulation thread).
/// The engine is created and started on a background queue: Core Audio device setup can block
/// on coreaudiod and must never stall the UI or emulation threads.
final class AudioOutput {
    let ring: OpaquePointer
    private let audioQueue = DispatchQueue(label: "replaynes.audio.setup", qos: .userInitiated)
    private var engine: AVAudioEngine?          // audioQueue only
    private var muted = true                    // emulation thread only
    private let lock = NSLock()
    private var _volume: Float = 0.8
    private var _latency: Double = 0
    private var _running = false
    private var configObserver: NSObjectProtocol?

    /// Requested Core Audio IO buffer (frames at the device rate). Small = low latency.
    static let ioBufferFrames: UInt32 = 256

    init() {
        // capacity ~170 ms, start after 25 ms buffered, skip ahead if > 100 ms buffered.
        ring = rn_ring_create(8192, 1200, 4800)!
    }

    deinit {
        if let configObserver { NotificationCenter.default.removeObserver(configObserver) }
        audioQueue.sync { engine?.stop() }
        rn_ring_destroy(ring)
    }

    func start() {
        audioQueue.async { [weak self] in self?.startOnQueue() }
    }

    private func startOnQueue() {
        engine?.stop()
        let engine = AVAudioEngine()
        let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: Double(RN_SAMPLE_RATE), channels: 1, interleaved: false)!
        let ring = self.ring
        let source = AVAudioSourceNode(format: fmt) { _, _, frameCount, abl -> OSStatus in
            let list = UnsafeMutableAudioBufferListPointer(abl)
            guard let first = list.first?.mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            rn_ring_pull(ring, first, frameCount)
            if list.count > 1 {
                for i in 1..<list.count {
                    list[i].mData?.assumingMemoryBound(to: Float.self).update(from: first, count: Int(frameCount))
                }
            }
            return noErr
        }
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: fmt)
        if let au = engine.outputNode.audioUnit {
            var frames = Self.ioBufferFrames
            AudioUnitSetProperty(au, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &frames, UInt32(MemoryLayout<UInt32>.size))
        }
        lock.lock(); engine.mainMixerNode.outputVolume = _volume; lock.unlock()
        engine.prepare()
        var ok = false
        do { try engine.start(); ok = true } catch { NSLog("ReplayNES: audio engine failed to start: \(error)") }
        self.engine = engine
        lock.lock(); _running = ok; _latency = engine.outputNode.presentationLatency; lock.unlock()
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            if let o = self.configObserver { NotificationCenter.default.removeObserver(o) }
            // Device change (headphones, sample rate): rebuild the graph.
            self.configObserver = NotificationCenter.default.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine, queue: nil) { [weak self] _ in
                self?.start()
            }
        }
    }

    var running: Bool { lock.lock(); defer { lock.unlock() }; return _running }

    var volume: Float {
        get { lock.lock(); defer { lock.unlock() }; return _volume }
        set {
            lock.lock(); _volume = newValue; lock.unlock()
            audioQueue.async { [weak self] in self?.engine?.mainMixerNode.outputVolume = newValue }
        }
    }

    var outputLatencySeconds: Double { lock.lock(); defer { lock.unlock() }; return _latency }

    /// Emulation thread only.
    func setMuted(_ m: Bool) {
        if m == muted { return }
        muted = m
        rn_ring_set_muted(ring, m ? 1 : 0)
    }

    /// Emulation thread only.
    func push(_ pcm: UnsafeBufferPointer<Int16>) {
        guard let base = pcm.baseAddress, pcm.count > 0, !muted else { return }
        rn_ring_push(ring, base, UInt32(pcm.count))
    }

    func stats() -> rn_audio_ring_stats {
        var st = rn_audio_ring_stats()
        rn_ring_get_stats(ring, &st)
        return st
    }
}
