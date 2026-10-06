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
    /// CPU time consumed by the calling thread (seconds).
    static func threadCPUSeconds() -> Double { Double(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)) / 1e9 }
    /// Sleeps until `seconds` (host-clock seconds, as CoreAnimation / Metal timestamps).
    static func wait(untilSeconds t: Double) {
        let target = ticks(seconds: t)
        if target > now() { mach_wait_until(target) }
    }
}

/// Timestamps (mach ticks) attached to a published frame for latency measurement.
struct FrameMeta {
    var frame: UInt64 = 0
    var inputEventTime: UInt64 = 0  // last physical input change consumed by this frame (0 = none)
    var sampleTime: UInt64 = 0      // rn_input_sample_game
    var emulatedTime: UInt64 = 0    // rn_step returned
    var tickStart: UInt64 = 0       // display callback / tick began (before the just-in-time input wait)
    var targetPresentation = 0.0    // refresh the frame was made for (display link, seconds; 0 = none)
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
        for o in observers { o.signal() }
        lock.unlock()
    }

    private var observers: [DispatchSemaphore] = []

    /// `s` is signalled after every publish / clear (the viewport's present thread).
    func addPublishObserver(_ s: DispatchSemaphore) { lock.lock(); observers.append(s); lock.unlock() }
    func removePublishObserver(_ s: DispatchSemaphore) { lock.lock(); observers.removeAll { $0 === s }; lock.unlock() }

    func clear() {
        lock.lock()
        for i in pixels.indices { pixels[i] = 0xFF00_0000 }
        meta = FrameMeta()
        seq &+= 1
        for o in observers { o.signal() }
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
        if m { resampler.reset(); rate.reset() }
    }

    // Dynamic rate control (emulation thread): frames are emulated at the display's rate, so the
    // stream is resampled by a ratio that keeps the ring level at `targetFill` (AudioRateControl).
    /// Ring level aimed at just before a frame's samples are pushed (~21 ms; the output starts
    /// after `prime` = 25 ms are buffered and settles here).
    static let targetFill = 1024.0
    private var rate = AudioRateControl(targetFill: AudioOutput.targetFill)
    private var resampler = AudioResampler()
    private var resampled: [Int16] = []
    /// Output / input sample ratio of the last pushed frame (emulation thread).
    private(set) var ratio = 1.0

    /// Emulation thread only. `frameRate`: frames per second the emulation currently runs at
    /// (the display-locked rate, or the NTSC rate on the host clock).
    func push(_ pcm: UnsafeBufferPointer<Int16>, frameRate: Double = 1 / FramePacing.period) {
        guard pcm.baseAddress != nil, pcm.count > 0, !muted else { return }
        var st = rn_audio_ring_stats()
        rn_ring_get_stats(ring, &st)
        rate.setFrameRate(frameRate)
        ratio = rate.update(fill: Double(st.fill))
        resampled.removeAll(keepingCapacity: true)
        resampler.process(pcm, ratio: ratio, into: &resampled)
        resampled.withUnsafeBufferPointer { b in
            if let base = b.baseAddress, b.count > 0 { rn_ring_push(ring, base, UInt32(b.count)) }
        }
    }

    func stats() -> rn_audio_ring_stats {
        var st = rn_audio_ring_stats()
        rn_ring_get_stats(ring, &st)
        return st
    }
}

extension HostClock {
    /// Makes the calling thread a time-constraint (real-time) thread, like Core Audio's IO threads:
    /// it is woken on time and run on a performance core even when the machine is otherwise idle
    /// (a lightly loaded Mac otherwise lets these periodic wakeups drift by several milliseconds,
    /// which shows as uneven frame pacing). `computation` is the CPU time needed per `period`,
    /// `constraint` the window it must finish in. The kernel demotes a thread that overruns.
    static func makeCurrentThreadRealtime(period: Double, computation: Double, constraint: Double) {
        var policy = thread_time_constraint_policy_data_t(
            period: UInt32(ticks(seconds: period)), computation: UInt32(ticks(seconds: computation)),
            constraint: UInt32(ticks(seconds: constraint)), preemptible: 1)
        let count = mach_msg_type_number_t(MemoryLayout<thread_time_constraint_policy_data_t>.size / MemoryLayout<integer_t>.size)
        let r = withUnsafeMutablePointer(to: &policy) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                thread_policy_set(pthread_mach_thread_np(pthread_self()), thread_policy_flavor_t(THREAD_TIME_CONSTRAINT_POLICY), $0, count)
            }
        }
        if r != KERN_SUCCESS { NSLog("ReplayNES: real-time thread policy not applied (\(r))") }
    }
}

extension UserDefaults {
    /// A boolean preference with a default. Launch arguments (`-key NO`) arrive as strings, which
    /// `object(forKey:) as? Bool` would not read; bool(forKey:) parses YES/NO/1/0/true/false.
    func flag(_ key: String, default value: Bool) -> Bool { object(forKey: key) == nil ? value : bool(forKey: key) }
}
