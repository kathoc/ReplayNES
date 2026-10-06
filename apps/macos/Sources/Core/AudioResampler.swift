// Streaming resampler for the live audio path (dynamic rate control, see AudioRateControl).
// 4-point Hermite (Catmull-Rom) interpolation on a continuous int16 mono stream: the ratio may
// change per chunk and the fractional position carries over, so chunk boundaries are seamless.
// Ratios stay within +-0.5 % of 1, where this interpolator is transparent for NES audio.
// Display path only: recordings and MP4 export use the emulator's samples untouched.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct AudioResampler {
    /// Last three input samples of the previous chunk (x[-3], x[-2], x[-1]).
    private var history: [Float] = [0, 0, 0]
    /// Position of the next output sample, in input samples relative to the start of the next
    /// chunk's x[-1] (0 = exactly on the last sample of the previous chunk).
    private var position = 0.0
    private var primed = false
    private var work: [Float] = []

    init() {}

    mutating func reset() {
        history = [0, 0, 0]
        position = 0
        primed = false
    }

    /// Resamples `input` with `ratio` output samples per input sample, appending to `out`.
    /// Output lags the input by one sample (interpolation needs one sample of look-ahead).
    mutating func process(_ input: UnsafeBufferPointer<Int16>, ratio: Double, into out: inout [Int16]) {
        guard input.count > 0 else { return }
        if !primed {
            // Start on the first sample (no ramp from silence).
            let first = Float(input[0])
            history = [first, first, first]
            primed = true
        }
        // work = x[-3], x[-2], x[-1], x[0] ... x[n-1]
        work.removeAll(keepingCapacity: true)
        work.append(contentsOf: history)
        for s in input { work.append(Float(s)) }
        let step = 1 / max(0.5, min(2, ratio))
        let n = work.count
        // Interpolate between work[i] and work[i+1] with neighbours work[i-1], work[i+2];
        // position 0 corresponds to work[2] (x[-1]).
        var p = position
        out.reserveCapacity(out.count + Int(Double(input.count) * ratio) + 2)
        while true {
            let i = Int(p.rounded(.down)) + 2
            if i + 2 >= n { break }
            let t = Float(p - p.rounded(.down))
            let y0 = work[i - 1], y1 = work[i], y2 = work[i + 1], y3 = work[i + 2]
            let c1 = 0.5 * (y2 - y0)
            let c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3
            let c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2)
            let v = ((c3 * t + c2) * t + c1) * t + y1
            out.append(Int16(max(-32768, min(32767, v.rounded()))))
            p += step
        }
        // Keep the last three samples; positions are re-based on the new x[-1].
        position = p - Double(input.count)
        history = [work[n - 3], work[n - 2], work[n - 1]]
    }
}
