// The CRT reception of a picture (no signal / power-on, docs/CRT_PORT.md "No signal and power-on")
// from the shared frontend core (rnf_crt_reception_*), as CRTRenderer.Reception. Display only.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

extension CRTRenderer.Reception {
    /// The reception of power-on step `step` (0 ..< RNF_CRT_POWER_ON_FRAMES) or
    /// RNF_CRT_RECEPTION_NO_SIGNAL at picture `counter`; nil = normal reception (the model untouched).
    init?(step: Int32, counter: UInt64, flashReduced: Bool, antennaDbuv: Double) {
        var r = rnf_crt_reception()
        rnf_crt_reception_at(step, counter, flashReduced ? 1 : 0, &r)
        guard r.active != 0 else { return nil }
        var x = rnf_crt_receiver()
        rnf_crt_reception_receiver(&r, CRTRF.carrierToNoiseDb(antennaDbuv), &x)
        self.init()
        signalLevel = x.signal_level
        minGain = x.agc_min_gain
        maxGain = x.agc_max_gain
        noiseFrame = x.noise_frame
        chroma = x.chroma
        lift = x.lift
        roll = r.roll
        tear = r.tear
        tearCycles = r.tear_cycles
        tearPhase = r.tear_phase
        jitter = r.jitter
        jitterKey = r.jitter_key
        picture = r.picture
        noSignal = r.no_signal != 0
    }
}

/// Which reception the shown pictures have (rnf_crt_power) and the hiss mixed into the game sound
/// while it lasts (rnf_crt_hiss). Emulation thread only.
final class CRTPowerTracker {
    private let power = rnf_crt_power_new()
    private let hiss = rnf_crt_hiss_new()
    deinit { rnf_crt_power_free(power); rnf_crt_hiss_free(hiss) }

    /// Reception step of the last picture (RNF_CRT_RECEPTION_* or a power-on step).
    var current: Int32 { rnf_crt_power_current(power) }
    var counter: UInt64 { rnf_crt_power_counter(power) }

    func reset() { rnf_crt_power_reset(power); rnf_crt_hiss_reset(hiss) }
    func start() { rnf_crt_power_start(power) }
    @discardableResult func step(from frameBefore: UInt64?, to frame: UInt64, events: UInt8) -> Int32 {
        rnf_crt_power_step(power, frameBefore ?? UInt64.max, frame, events)
    }
    @discardableResult func show(_ frame: UInt64) -> Int32 { rnf_crt_power_show(power, frame) }
    @discardableResult func tick() -> Int32 { rnf_crt_power_tick(power) }

    /// Mixes the hiss of the current picture's reception into `pcm` (a copy of the frame's sound);
    /// returns false when there is none (the sound is untouched).
    func mixHiss(_ pcm: inout [Int16], flashReduced: Bool) -> Bool {
        var r = rnf_crt_reception()
        rnf_crt_reception_at(current, counter, flashReduced ? 1 : 0, &r)
        if r.hiss == 0 && rnf_crt_hiss_level(hiss) == 0 { return false }
        pcm.withUnsafeMutableBufferPointer { rnf_crt_hiss_mix(hiss, $0.baseAddress, $0.count, r.hiss) }
        return true
    }
}
