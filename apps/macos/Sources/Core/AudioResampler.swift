// Streaming resampler for the live audio path (dynamic rate control, see AudioRateControl):
// 4-point Hermite on a continuous int16 mono stream, implemented by the shared frontend core
// (rnf_resampler). Display path only: recordings and MP4 export use the emulator's samples.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct AudioResampler {
    private var box = RNFHandle(rnf_resampler_new(), free: { rnf_resampler_free($0) }, clone: { rnf_resampler_clone($0) })
    private var scratch: [Int16] = []

    init() {}

    mutating func reset() { rnf_resampler_reset(RNFHandle.unique(&box)) }

    /// Resamples `input` with `ratio` output samples per input sample, appending to `out`.
    /// Output lags the input by one sample (interpolation needs one sample of look-ahead).
    mutating func process(_ input: UnsafeBufferPointer<Int16>, ratio: Double, into out: inout [Int16]) {
        guard input.count > 0, let base = input.baseAddress else { return }
        let bound = rnf_resampler_output_bound(input.count)
        if scratch.count < bound { scratch = [Int16](repeating: 0, count: bound) }
        let h = RNFHandle.unique(&box)
        var n = 0
        scratch.withUnsafeMutableBufferPointer { buf in
            _ = rnf_resampler_process(h, base, input.count, ratio, buf.baseAddress, bound, &n)
        }
        out.append(contentsOf: scratch[0..<n])
    }
}
