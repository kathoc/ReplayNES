// Physical CRT model ported from nesterm (kathoc, MIT) vendor/crt — itself exported from the
// "crt-physical-model" project (EXPORT-MANIFEST commit 919c1563). This file holds the CPU-side
// SETUP math only (constants, FIR design, detector/beam tables, phosphor fractions), ported 1:1
// from the reference modules; every per-frame operation runs on the GPU (CRTShaders.swift).
// It is an uncalibrated, assumption-labelled model (ledger IDs kept in comments), not a measured
// television. Mapping and deviations: docs/CRT_PORT.md.
// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md
import Foundation

// MARK: - reference/composite.mjs (signalFixture, signalVoltage)

enum CRTComposite {
    static let width = 256, height = 240, samplesPerDot = 8, lineSamples = 2728
    static let frameSamples = 341 * 262 * 8
    static let sampleClockNumerator = 945_000_000.0, sampleClockDenominator = 22.0
    static let carrierSamples = 12
    static let activeStart = 36, activeEnd = 2084, syncStart = 2244, syncEnd = 2444
    static let burstStart = 2476, burstEnd = 2596, clampStart = 2172, clampEnd = 2244
    static let blank = 0.312, white = 1.100, sync = 0.048, burstDC = 0.336
    static let burstLow = 0.148, burstHigh = 0.524
    static let driveExponent = 2.2
    static let low: [Double] = [0.228, 0.312, 0.552, 0.880], high: [Double] = [0.616, 0.840, 1.100, 1.100]
    static let attLow: [Double] = [0.192, 0.256, 0.448, 0.712], attHigh: [Double] = [0.500, 0.676, 0.896, 0.896]

    @inline(__always) static func mod12(_ n: Int) -> Int { ((n % 12) + 12) % 12 }
    @inline(__always) static func highPhase(_ hue: Int, _ phase: Int) -> Bool { mod12(hue + phase) < 6 }

    /// Composite voltage of a 9-bit PPU code at carrier phase (NES-M2-LEVELS).
    static func signalVoltage(_ code: Int, _ phase: Int) -> Double {
        let hue = code & 15, level = (code >> 4) & 3, emphasis = code >> 6
        if hue >= 14 { return 0.312 }
        let attenuated = ((emphasis & 1) != 0 && highPhase(12, phase)) ||
            ((emphasis & 2) != 0 && highPhase(4, phase)) || ((emphasis & 4) != 0 && highPhase(8, phase))
        let isHigh = hue == 0 || (hue != 13 && highPhase(hue, phase))
        return (isHigh ? (attenuated ? attHigh : high) : (attenuated ? attLow : low))[level]
    }

    /// rf-webgl.mjs voltageTexture: lut[phase*512 + code] (float32).
    static let voltageLUT: [Float] = {
        var lut = [Float](repeating: 0, count: 512 * 12)
        for phase in 0..<12 { for code in 0..<512 { lut[phase * 512 + code] = Float(signalVoltage(code, phase)) } }
        return lut
    }()

    /// Per-row carrier phase (rf-webgl.mjs renderCodes phaseUpload) for a frame whose line 0
    /// starts at `lineStart0` samples (core-observed PPU ticks x 8, "assumed-relative" origin).
    static func rowPhases(lineStart0: Int, initialPhase: Int = 0) -> [Int32] {
        (0..<240).map { row in Int32(mod12(mod12(lineStart0 + row * lineSamples) - activeStart + initialPhase)) }
    }

    /// ReplayNES adapter (replaces nesterm's installCodeTap tick counter): Nestopia's colour-burst
    /// phase b (0..2) advances by 1 per 89342-dot frame and by 2 per 89341-dot (skipped-dot) frame,
    /// i.e. b = -(PPU dots) mod 3 up to a constant, so 8*dots mod 12 = 4*b (same relative origin
    /// freedom as nesterm's phaseOrigin 'assumed-relative'). Row 0 sample start = 36 + 4*b.
    static func rowPhases(burstPhase b: UInt32) -> [Int32] {
        rowPhases(lineStart0: activeStart + 4 * Int(b % 3))
    }
}

// MARK: - reference/rf.mjs (rfFixture, designIF, noise)

enum CRTRF {
    static let sampleRateHz = CRTComposite.sampleClockNumerator / CRTComposite.sampleClockDenominator
    static let modulationDepth = 0.875, taps = 1025, vestigeHz = 750_000.0
    static let cutoffHz = 4_200_000.0, transitionHz = 600_000.0, quadratureBins = 4096
    static let maxSamples = CRTComposite.lineSamples * CRTComposite.height  // 654720
    static let fftSize = 4096

    struct Filter { let real: [Double]; let imag: [Double]; let delaySamples: Int }

    /// designIF(): asymmetric (vestigial) IF response by midpoint quadrature of the inverse DTFT,
    /// Blackman window, unity complex carrier gain (M3-IF). Cached: fixed parameters.
    static let filter: Filter = designIF()

    static func designIF() -> Filter {
        let fs = sampleRateHz, delay = (taps - 1) / 2
        var real = [Double](repeating: 0, count: taps), imag = [Double](repeating: 0, count: taps)
        let tau = 2 * Double.pi
        for b in 0..<quadratureBins {
            let frequency = fs * ((Double(b) + 0.5) / Double(quadratureBins) - 0.5), af = abs(frequency)
            if af >= cutoffHz { continue }
            let edge = af <= cutoffHz - transitionHz ? 1 : 0.5 * (1 + cos(Double.pi * (af - cutoffHz + transitionHz) / transitionHz))
            let nyquist = frequency <= -vestigeHz ? 0 : frequency >= vestigeHz ? 1 : 0.5 + 0.5 * sin(Double.pi * frequency / (2 * vestigeHz))
            let h = nyquist * edge / Double(quadratureBins)
            for j in 0..<taps {
                let angle = tau * frequency * Double(j - delay) / fs
                real[j] += h * cos(angle); imag[j] += h * sin(angle)
            }
        }
        var gr = 0.0, gi = 0.0
        for j in 0..<taps {
            let w = 0.42 - 0.5 * cos(tau * Double(j) / Double(taps - 1)) + 0.08 * cos(2 * tau * Double(j) / Double(taps - 1))
            real[j] *= w; imag[j] *= w; gr += real[j]; gi += imag[j]
        }
        let norm = gr * gr + gi * gi
        for j in 0..<taps {
            let r = real[j], i = imag[j]
            real[j] = (r * gr + i * gi) / norm; imag[j] = (i * gr - r * gi) / norm
        }
        return Filter(real: real, imag: imag, delaySamples: delay)
    }

    /// rf-webgl.mjs kernelSpectrum(): forward FFT (double) of the zero-padded FIR, float32 out.
    static let kernelSpectrum: [SIMD2<Float>] = spectrum(real: filter.real, imag: filter.imag)

    /// Fast path: spectrum of Re(h) alone. The receiver only reads the real part of the IF output,
    /// and for a real input signal Re(signal * h) = signal * Re(h), a real FIR.
    static let kernelSpectrumReal: [SIMD2<Float>] = spectrum(real: filter.real, imag: [Double](repeating: 0, count: filter.real.count))

    private static func spectrum(real: [Double], imag: [Double]) -> [SIMD2<Float>] {
        let n = fftSize
        var r = [Double](repeating: 0, count: n), q = [Double](repeating: 0, count: n)
        for i in 0..<real.count { r[i] = real[i]; q[i] = imag[i] }
        var j = 0
        for i in 1..<n {
            var bit = n >> 1
            while j & bit != 0 { j ^= bit; bit >>= 1 }
            j ^= bit
            if i < j { r.swapAt(i, j); q.swapAt(i, j) }
        }
        var w = 2
        while w <= n {
            var a = 0
            while a < n {
                for k in 0..<(w / 2) {
                    let c = cos(-2 * Double.pi * Double(k) / Double(w)), s = sin(-2 * Double.pi * Double(k) / Double(w))
                    let b = a + k + w / 2, kk = a + k
                    let tr = r[b] * c - q[b] * s, ti = r[b] * s + q[b] * c
                    r[b] = r[kk] - tr; q[b] = q[kk] - ti; r[kk] += tr; q[kk] += ti
                }
                a += w
            }
            w *= 2
        }
        return (0..<n).map { SIMD2(Float(r[$0]), Float(q[$0])) }
    }

    /// rf-webgl.mjs twiddles: (cos, sin)(2*pi*i/N) as float32.
    static let twiddles: [SIMD2<Float>] = (0..<fftSize).map {
        SIMD2(Float(cos(2 * Double.pi * Double($0) / Double(fftSize))), Float(sin(2 * Double.pi * Double($0) / Double(fftSize))))
    }

    static var overlap: Int { taps - 1 }
    static var hop: Int { fftSize - overlap }
    static var blocks: Int { (maxSamples + hop - 1) / hop }

    // M3-NOISE (assumed): kTB + NF in a 4.2 MHz bandwidth, 75 ohm.
    static let noiseBandwidthHz = 4.2e6, noiseFigureDb = 7.0, defaultInputDbuv = 65.0
    static func carrierToNoiseDb(_ inputDbuv: Double) -> Double {
        inputDbuv - (-174 + 10 * log10(noiseBandwidthHz) + noiseFigureDb + 108.75)
    }
    static func noiseSigma(cnrDb: Double, signalLevel: Double) -> Double {
        let gain = sqrt(filter.real.reduce(0) { $0 + $1 * $1 })
        return signalLevel * pow(10, -cnrDb / 20) / gain
    }
    /// lowbias32 integer hash (identical in MSL).
    static func hash32(_ v: UInt32) -> UInt32 {
        var x = v
        x ^= x >> 16; x = x &* 0x7feb352d; x ^= x >> 15; x = x &* 0x846ca68b; x ^= x >> 16
        return x
    }
    /// noiseKey(seed, frame): hash32((seed ^ hash32(frame)) >>> 0); `frame` is a JS number whose
    /// low 32 bits feed hash32 (x >>>= 0).
    static func noiseKey(seed: UInt32, frame: UInt64) -> Int32 {
        Int32(bitPattern: hash32(seed ^ hash32(UInt32(truncatingIfNeeded: frame))))
    }
}

// MARK: - reference/agc.mjs (GatedAGC defaults; the per-row loop runs on the GPU)

enum CRTAGC {
    static let initialGain = 1.0, attackSeconds = 0.002, releaseSeconds = 0.020
    static let minGain = 1.0 / 16, maxGain = 16.0
}

// MARK: - reference/phosphor.mjs

enum CRTPhosphor {
    static let nesLineS = 341.0 * 4 / 21_477_272
    static let nesFrameS = 262 * nesLineS
    static let cutoffS = 0.1
    struct Channel { let exponentials: [(Double, Double)]; let power: (a: Double, alpha: Double, beta: Double)? }
    // M4A-DECAY: Kuhn 2002 eqs (8)-(10) fitted to one VGA monitor (inferred).
    static let channels: [Channel] = [
        Channel(exponentials: [(4, 360), (1.75, 1.6e3), (2, 8e3), (2.25, 25e3), (15, 700e3), (29, 7e6)], power: nil),
        Channel(exponentials: [(37, 150e3), (100, 700e3), (90, 5e6)], power: (210e-6, 5.5e-6, 1.1)),
        Channel(exponentials: [(75, 100e3), (1000, 1.1e6), (1100, 4e6)], power: (190e-6, 5e-6, 1.11)),
    ]
    static func rawEnergy(_ ch: Channel, _ t: Double) -> Double {
        if t <= 0 { return 0 }
        var sum = 0.0
        for (amplitude, frequency) in ch.exponentials {
            let tau = 1 / (2 * Double.pi * frequency)
            sum += amplitude * tau * -expm1(-t / tau)
        }
        if let p = ch.power { sum += p.a / (p.beta - 1) * (pow(p.alpha, 1 - p.beta) - pow(t + p.alpha, 1 - p.beta)) }
        return sum
    }
    static func cumulativeEnergy(_ c: Int, _ t: Double, cutoff: Double = cutoffS) -> Double {
        let ch = channels[c]
        return min(t, cutoff) <= 0 ? 0 : rawEnergy(ch, min(t, cutoff)) / rawEnergy(ch, cutoff)
    }
    /// F(m): energy fraction of a frame-m-ago impulse falling in the current frame window.
    static func frameFractions(_ c: Int, framePeriod: Double = nesFrameS, cutoff: Double = cutoffS) -> [Double] {
        let count = Int((cutoff / framePeriod).rounded(.up))
        return (0..<count).map { m in
            cumulativeEnergy(c, Double(m + 1) * framePeriod, cutoff: cutoff) - cumulativeEnergy(c, Double(m) * framePeriod, cutoff: cutoff)
        }
    }
    static let fractions: [[Double]] = (0..<3).map { frameFractions($0) }
    static var depth: Int { fractions[0].count }
}

// MARK: - reference/supply.mjs (M4C, assumed values)

enum CRTSupply {
    static let v0 = 25_000.0, reff = 0.4e6, tauS = 1.5e-3, imax = 1.2e-3, ilim = 0.8e-3, tauAblS = 0.3, n = 1.75
    static let share: [Double] = [0.42, 0.30, 0.28]
    static let lineS = CRTPhosphor.nesLineS, frameS = CRTPhosphor.nesFrameS
    static let visibleLines = 240.0, totalLines = 262.0
}

// MARK: - reference/tube.mjs + reference/spot-h.mjs

enum CRTTube {
    // fixture 'm1-synthetic-2' (M1-GEOMETRY / SLOT / BEAM / GAIN / SCATTER / AMBIENT / PRESENTATION)
    static let surfaceWidthM = 0.28, surfaceHeightM = 0.21
    static let triadPitchM = 0.00075, widthFraction = 0.22, verticalPeriodM = 0.001, verticalFill = 0.8, staggerFraction = 0.5
    static let sigmaXPitches = 0.42, sigmaYPitches = 0.32, cutoffSigma = 4.0
    static let growthDiscPitches = 1.22, growthLevels = 33
    static let emissionGain = 0.75
    static let scatterFraction = 0.12, scatterSigmaM = 0.002
    static let albedo = 0.10, diffuseFraction = 0.65, normalSlopeX = 0.4, normalSlopeY = 0.3
    static let lightX = -0.3, lightY = -0.4, lightZ = 1.0, referenceWhiteCdM2 = 80.0
    static let sigmaOutputPixels = sqrt(2 * log(100.0)) / Double.pi
    static let reconstructionCutoffSigma = 4.0
    static let truncatedGaussianMass = 0.9999366575163338
    static let spotHRadius = 3  // M4C-SPOT-H

    struct Panel { let center: Double; let length: Double; let periodIndex: Int }

    @inline(__always) static func mod(_ x: Int, _ p: Int) -> Int { ((x % p) + p) % p }

    static func exposedPanels(_ low: Double, _ high: Double, _ period: Double, _ center: Double, _ openWidth: Double) -> [Panel] {
        var result: [Panel] = []
        var k = Int(((low - center - openWidth / 2) / period).rounded(.down))
        let kEnd = Int(((high - center + openWidth / 2) / period).rounded(.up))
        while k <= kEnd {
            let left = max(low, center + Double(k) * period - openWidth / 2)
            let right = min(high, center + Double(k) * period + openWidth / 2)
            if right > left { result.append(Panel(center: (left + right) / 2, length: right - left, periodIndex: k)) }
            k += 1
        }
        return result
    }

    static func weights(_ position: Double, _ size: Int, _ sigma: Double, normalize: Bool) -> [(Int, Double)] {
        var result: [(Int, Double)] = []
        var sum = 0.0
        let radius = cutoffSigma * sigma
        let lo = max(0, Int((position - radius).rounded(.up))), hi = min(size - 1, Int((position + radius).rounded(.down)))
        if lo <= hi {
            for i in lo...hi {
                let d = (position - Double(i)) / sigma
                let w = exp(-0.5 * (d * d)) / (sqrt(2 * Double.pi) * sigma)
                result.append((i, w)); sum += w
            }
        }
        if normalize { for k in result.indices { result[k].1 /= sum } }
        return result
    }

    /// Continuous Gaussian detector integrated over exposed slot material (M1-PRESENTATION).
    static func detectorQuadrature(_ outputCount: Int, _ length: Double, _ samples: Int, horizontal: Bool,
                                   _ visit: (Int, Int, Double, Double) -> Void) {
        let pixel = length / Double(outputCount)
        let sigma = sigmaOutputPixels * pixel, radius = reconstructionCutoffSigma * sigma
        let step = pixel / Double(samples), norm = sqrt(2 * Double.pi) * sigma * truncatedGaussianMass
        for p in 0..<outputCount {
            let center = (Double(p) + 0.5) * pixel, low = max(0, center - radius), high = min(length, center + radius)
            for branch in 0..<(horizontal ? 3 : 2) {
                let period = horizontal ? triadPitchM : verticalPeriodM
                let apertureCenter = horizontal ? (Double(branch) + 0.5) / 3 * period : (0.5 - Double(branch) * staggerFraction) * period
                let apertureWidth = period * (horizontal ? widthFraction : verticalFill)
                for exposed in exposedPanels(low, high, period, apertureCenter, apertureWidth) {
                    let count = Int((exposed.length / step).rounded(.up)), delta = exposed.length / Double(count)
                    let target = horizontal ? branch * 2 + mod(exposed.periodIndex, 2) : branch
                    for q in 0..<count {
                        let coordinate = exposed.center - exposed.length / 2 + (Double(q) + 0.5) * delta
                        let d = (coordinate - center) / sigma
                        visit(p, target, coordinate, exp(-0.5 * (d * d)) * delta / norm)
                    }
                }
            }
        }
    }

    /// detectorMaps(): per output pixel and branch, (input index, weight) in first-insertion
    /// order (JS Map semantics, which fixes the GPU summation order).
    static func detectorMaps(_ outputCount: Int, _ inputCount: Int, _ length: Double, _ samples: Int, horizontal: Bool,
                             sigmaX: Double, sigmaY: Double) -> [[[(Int, Double)]]] {
        let branches = horizontal ? 6 : 2
        var maps = [[[(Int, Double)]]](repeating: [[(Int, Double)]](repeating: [], count: branches), count: outputCount)
        var slot = [Int](repeating: -1, count: inputCount)  // index -> position in the current list
        var lastKey = (-1, -1)
        detectorQuadrature(outputCount, length, samples, horizontal: horizontal) { p, target, coordinate, detector in
            if lastKey != (p, target) {
                // Rebuild the lookup for this (p,target) list (lists are visited contiguously per p).
                for i in slot.indices { slot[i] = -1 }
                for (k, e) in maps[p][target].enumerated() { slot[e.0] = k }
                lastKey = (p, target)
            }
            for (i, beam) in weights(coordinate / length * Double(inputCount) - 0.5, inputCount,
                                     horizontal ? sigmaX : sigmaY, normalize: horizontal) {
                if slot[i] >= 0 { maps[p][target][slot[i]].1 += beam * detector }
                else { slot[i] = maps[p][target].count; maps[p][target].append((i, beam * detector)) }
            }
        }
        return maps
    }

    static func growthSigma(_ u: Double, _ mass: Double, _ growth: Double) -> Double {
        let disc = growth * growthDiscPitches
        return mass * sqrt(sigmaYPitches * sigmaYPitches + disc * disc * u / 16)
    }

    struct GrowthTable { let first: Int; let count: Int; let values: [Double] }

    /// growthTables(): K-level vertical tables over a common row support (M4A-SPOT-LAW).
    static func growthTables(_ oh: Int, _ height: Int, _ samples: Int, _ mass: Double, _ growth: Double, _ levels: Int) -> [[GrowthTable]] {
        let maxSigma = growthSigma(1, mass, growth)
        var points = [[[(Double, Double)]]](repeating: [[], []], count: oh)
        detectorQuadrature(oh, surfaceHeightM, samples, horizontal: false) { p, target, coordinate, detector in
            points[p][target].append((coordinate / surfaceHeightM * Double(height) - 0.5, detector))
        }
        let sigmas = (0..<levels).map { growthSigma(levels == 1 ? 0 : Double($0) / Double(levels - 1), mass, growth) }
        return points.map { rows in rows.map { pts -> GrowthTable in
            if pts.isEmpty { return GrowthTable(first: 0, count: 0, values: []) }
            let lo = max(0, Int((pts.map { $0.0 }.min()! - cutoffSigma * maxSigma).rounded(.up)))
            let hi = min(height - 1, Int((pts.map { $0.0 }.max()! + cutoffSigma * maxSigma).rounded(.down)))
            let count = max(0, hi - lo + 1)
            var values = [Double](repeating: 0, count: levels * count)
            for k in 0..<levels {
                for (position, detector) in pts {
                    for (iy, w) in weights(position, height, sigmas[k], normalize: false) where iy >= lo && iy <= hi {
                        values[k * count + iy - lo] += w * detector
                    }
                }
            }
            return GrowthTable(first: lo, count: count, values: values)
        } }
    }

    /// M4C-SPOT-H: extra horizontal sigma in drive samples for current fraction u.
    static func extraSigmaSamples(_ u: Double, growth: Double, width: Int, scanLines: Int = 240) -> Double {
        let pitchM = surfaceHeightM / Double(scanLines), sampleM = surfaceWidthM / Double(width)
        return growth * growthDiscPitches * pitchM * sqrt(max(0, u)) / 4 / sampleM
    }

    /// createTubePlan() result, packed for the GPU exactly like tube-webgl.mjs (packMaps /
    /// packGrowth / colsum / kernels / ambient).
    struct Plan {
        let width: Int, height: Int, outputWidth: Int, outputHeight: Int
        let gain: Double, scatterFraction: Double
        let xCount: Int; let xmap: [SIMD2<Float>]          // [(x*6+b)*xCount + k] = (index, weight)
        let yCount: Int; let ymap: [SIMD2<Float>]          // fixed spot: [(y*2+parity)*yCount + k]
        let growthLevels: Int; let gCount: Int; let gmap: [SIMD2<Float>]  // [(y*2+parity)*(levels*gCount) + k*gCount + j]
        let colsum: [Float]                                 // [b*ow + x]
        let kernels: [(radius: Int, weights: [Float])]      // scatter x, y
        let ambient: [Float]                                // ow*oh (same value in r,g,b)
        let growth: Bool
    }

    /// Fast path tables, repacked from a Plan: horizontal taps whose padding points at the tile's
    /// first input (weight 0) plus the input span of every 64-column tile; vertical spot tables as
    /// contiguous input rows (first, count) per output row and parity with, per tap, a cubic in the
    /// beam-current fraction u = h / colsum least-squares fitted to the 33 growth-level tables
    /// (max error ~5e-4 of the row's largest weight; fixed spot: the constant weight, duplicate
    /// input rows of the detector map merged); and 1 / colsum per branch column (0 if colsum <= 0).
    struct FastPlan {
        let xmap: [SIMD2<Float>]; let tiles: [SIMD2<Int32>]; let maxSpan: Int
        let vrows: [SIMD2<Int32>]; let vcoef: [SIMD4<Float>]; let taps: Int
        let colinv: [SIMD4<Float>]   // [parity * ow + x] = 1 / colsum of branches (c * 2 + parity), c = 0...2
    }

    /// Least-squares cubic through (l / (n - 1), y[l]).
    static func cubicFit(_ y: [Double]) -> SIMD4<Float> {
        let n = y.count
        if n < 2 { return SIMD4(Float(y.first ?? 0), 0, 0, 0) }
        var a = [[Double]](repeating: [Double](repeating: 0, count: 5), count: 4)
        for i in 0..<n {
            let u = Double(i) / Double(n - 1)
            let p = [1, u, u * u, u * u * u]
            for r in 0..<4 { for c in 0..<4 { a[r][c] += p[r] * p[c] }; a[r][4] += p[r] * y[i] }
        }
        for c in 0..<4 {
            var piv = c
            for r in c..<4 where abs(a[r][c]) > abs(a[piv][c]) { piv = r }
            a.swapAt(c, piv)
            for r in 0..<4 where r != c {
                let f = a[r][c] / a[c][c]
                for k in c...4 { a[r][k] -= f * a[c][k] }
            }
        }
        return SIMD4((0..<4).map { Float(a[$0][4] / a[$0][$0]) })
    }

    static func fastPlan(_ p: Plan) -> FastPlan {
        let ow = p.outputWidth, oh = p.outputHeight, n = p.xCount
        var xmap = p.xmap
        var tiles: [SIMD2<Int32>] = []
        var maxSpan = 0
        for tile in 0..<((ow + 63) / 64) {
            let cols = (tile * 64)..<min(ow, tile * 64 + 64)
            var lo = Int.max, hi = Int.min
            for x in cols { for b in 0..<6 { for k in 0..<n where xmap[(x * 6 + b) * n + k].y != 0 {
                lo = min(lo, Int(xmap[(x * 6 + b) * n + k].x)); hi = max(hi, Int(xmap[(x * 6 + b) * n + k].x))
            } } }
            if lo > hi { lo = 0; hi = 0 }
            for x in cols { for b in 0..<6 { for k in 0..<n where xmap[(x * 6 + b) * n + k].y == 0 {
                xmap[(x * 6 + b) * n + k] = SIMD2(Float(lo), 0)
            } } }
            tiles.append(SIMD2(Int32(lo), Int32(hi)))
            maxSpan = max(maxSpan, hi - lo + 1)
        }
        var vrows: [SIMD2<Int32>] = [], vcoef: [SIMD4<Float>] = []
        var taps = 1
        if p.growth {
            taps = p.gCount
            let L = p.growthLevels, rowWidth = L * p.gCount
            for row in 0..<(oh * 2) {
                let base = row * rowWidth
                var count = 0
                while count < p.gCount && p.gmap[base + count].x >= 0 { count += 1 }
                vrows.append(SIMD2(Int32(count > 0 ? p.gmap[base].x : 0), Int32(count)))
                for k in 0..<p.gCount {
                    vcoef.append(k < count ? cubicFit((0..<L).map { Double(p.gmap[base + $0 * p.gCount + k].y) }) : .zero)
                }
            }
        } else {
            var rows: [[Float]] = [], firsts: [Int] = []
            for row in 0..<(oh * 2) {
                let e = (0..<p.yCount).map { p.ymap[row * p.yCount + $0] }.filter { $0.y != 0 }
                guard let lo = e.map({ Int($0.x) }).min(), let hi = e.map({ Int($0.x) }).max() else { rows.append([]); firsts.append(0); continue }
                var w = [Float](repeating: 0, count: hi - lo + 1)
                for v in e { w[Int(v.x) - lo] += v.y }
                rows.append(w); firsts.append(lo)
            }
            taps = max(1, rows.map { $0.count }.max() ?? 1)
            for (row, w) in rows.enumerated() {
                vrows.append(SIMD2(Int32(firsts[row]), Int32(w.count)))
                for k in 0..<taps { vcoef.append(SIMD4(k < w.count ? w[k] : 0, 0, 0, 0)) }
            }
        }
        var colinv = [SIMD4<Float>](repeating: .zero, count: 2 * ow)
        if p.growth {
            for parity in 0..<2 { for x in 0..<ow { for c in 0..<3 {
                let cs = p.colsum[(c * 2 + parity) * ow + x]
                colinv[parity * ow + x][c] = cs > 0 ? 1 / cs : 0
            } } }
        }
        return FastPlan(xmap: xmap, tiles: tiles, maxSpan: maxSpan, vrows: vrows, vcoef: vcoef, taps: taps, colinv: colinv)
    }

    /// Fast path scatter source: the scattered light evaluated on the tube drive (512 x lines).
    /// The 2 mm scatter Gaussian is ~8x wider than the beam spot, the detector and the slot pitch,
    /// so blur(emission) = kappa * blur'(drive), blur' with the variances of scatter, beam spot and
    /// detector minus the bilinear upsampling tent (per axis, in drive samples), kappa = the mean
    /// emission per unit drive of the plan (flat field, per phosphor) times the scatter fraction.
    struct DriveScatter { let rx: Int; let ry: Int; let wx: [Float]; let wy: [Float]; let kappa: SIMD3<Float> }

    static func driveScatter(_ p: Plan) -> DriveScatter {
        let rows = Double(p.height), mass = rows / 240
        let sx = scatterSigmaM / (surfaceWidthM / 512), sy = scatterSigmaM / (surfaceHeightM / rows)
        let bx = sigmaXPitches * 512 / 256, by = sigmaYPitches * mass
        let dx = sigmaOutputPixels * 512 / Double(p.outputWidth), dy = sigmaOutputPixels * rows / Double(p.outputHeight)
        let sigX = (sx * sx + bx * bx + dx * dx - 1.0 / 6).squareRoot(), sigY = (sy * sy + by * by + dy * dy - 1.0 / 6).squareRoot()
        let ry = Int((4 * sigY).rounded(.up)), rx = Int((4 * sigX).rounded(.up))
        func kernel(_ sigma: Double, _ r: Int) -> [Float] {
            let w = (-r...r).map { d -> Double in let x = Double(d) / sigma; return exp(-0.5 * x * x) }
            let t = w.reduce(0, +)
            return w.map { Float($0 / t) }
        }
        // Flat field d: h = d * colsum, emission = gain * d * sum_parity colsum(x) * V(y), V = the
        // vertical weights at beam-current fraction d (growth) - separable, so the mean is cheap.
        let d = 0.5, ow = p.outputWidth, oh = p.outputHeight
        var kappa = SIMD3<Float>(0, 0, 0)
        for c in 0..<3 {
            var total = 0.0
            for parity in 0..<2 {
                var meanX = 0.0
                if p.growth { for x in 0..<ow { meanX += Double(p.colsum[(c * 2 + parity) * ow + x]) } }
                else { for x in 0..<ow { for k in 0..<p.xCount { meanX += Double(p.xmap[(x * 6 + c * 2 + parity) * p.xCount + k].y) } } }
                meanX /= Double(ow)
                var meanY = 0.0
                for y in 0..<oh {
                    let row = y * 2 + parity
                    if p.growth {
                        let L = p.growthLevels, G = p.gCount, t = d * Double(L - 1), kk = min(L - 2, Int(t)), f = t - Double(kk)
                        for k in 0..<G where p.gmap[row * L * G + k].x >= 0 {
                            meanY += Double(p.gmap[row * L * G + kk * G + k].y) * (1 - f) + Double(p.gmap[row * L * G + (kk + 1) * G + k].y) * f
                        }
                    } else { for k in 0..<p.yCount { meanY += Double(p.ymap[row * p.yCount + k].y) } }
                }
                total += meanX * meanY / Double(oh)
            }
            kappa[c] = Float(total * p.gain * p.scatterFraction)
        }
        return DriveScatter(rx: rx, ry: ry, wx: kernel(sigX, rx), wy: kernel(sigY, ry), kappa: kappa)
    }

    /// createTubePlan(input {width: 512, height: lines, beamReferenceWidth: 256, beamReferenceHeight: 240},
    /// {outputWidth, outputHeight, samples: 4, ambientLux, beamGrowth}) as used by physical-worker.mjs.
    static func makePlan(width: Int = 512, lines: Int, outputWidth ow: Int, outputHeight oh: Int, samples: Int = 4,
                         ambientLux: Double = 40, beamGrowth: Double = 1, beamReferenceWidth: Int = 256,
                         beamReferenceHeight: Int = 240) -> Plan {
        let height = lines
        let sigmaX = sigmaXPitches * Double(width) / Double(beamReferenceWidth)
        let mass = Double(height) / Double(beamReferenceHeight), sigmaY = sigmaYPitches * mass
        let xs = detectorMaps(ow, width, surfaceWidthM, samples, horizontal: true, sigmaX: sigmaX, sigmaY: sigmaY)
        let ys = detectorMaps(oh, height, surfaceHeightM, samples, horizontal: false, sigmaX: sigmaX, sigmaY: sigmaY)
        func pack(_ maps: [[[(Int, Double)]]]) -> (Int, [SIMD2<Float>]) {
            let branches = maps[0].count
            let count = max(1, maps.flatMap { $0 }.map { $0.count }.max() ?? 1)
            var data = [SIMD2<Float>](repeating: .zero, count: count * maps.count * branches)
            for p in maps.indices { for b in 0..<branches { for (k, e) in maps[p][b].enumerated() {
                data[(p * branches + b) * count + k] = SIMD2(Float(e.0), Float(e.1))
            } } }
            return (count, data)
        }
        let (xCount, xmap) = pack(xs), (yCount, ymap) = pack(ys)
        var gCount = 1, gmap: [SIMD2<Float>] = [], colsum = [Float](repeating: 0, count: ow * 6)
        let growth = beamGrowth > 0
        let levels = growthLevels
        if growth {
            let tables = growthTables(oh, height, samples, mass, beamGrowth, levels)
            gCount = max(1, tables.flatMap { $0 }.map { $0.count }.max() ?? 1)
            gmap = [SIMD2<Float>](repeating: .zero, count: levels * gCount * oh * 2)
            for (y, pair) in tables.enumerated() { for (parity, t) in pair.enumerated() {
                for k in 0..<levels { for j in 0..<gCount {
                    let i = (y * 2 + parity) * levels * gCount + k * gCount + j
                    gmap[i] = j < t.count ? SIMD2(Float(t.first + j), Float(t.values[k * t.count + j])) : SIMD2(-1, 0)
                } }
            } }
            for x in 0..<ow { for b in 0..<6 { colsum[b * ow + x] = Float(xs[x][b].reduce(0) { $0 + $1.1 }) } }
        }
        let kernels: [(radius: Int, weights: [Float])] = [ow, oh].enumerated().map { axis, count in
            let sigma = scatterSigmaM / ((axis == 1 ? surfaceHeightM : surfaceWidthM) / Double(count))
            let radius = Int((4 * sigma).rounded(.up))
            var w = [Double](repeating: 0, count: radius * 2 + 1)
            var total = 0.0
            for d in -radius...radius { let x = Double(d) / sigma; w[d + radius] = exp(-0.5 * (x * x)); total += w[d + radius] }
            return (radius, w.map { Float($0 / total) })
        }
        // renderRawTube(powered: false): smooth ambient reflection at final pixel centres.
        var ambient = [Float](repeating: 0, count: ow * oh)
        let lightNorm = sqrt(lightX * lightX + lightY * lightY + lightZ * lightZ)
        for y in 0..<oh { for x in 0..<ow {
            let nx = ((Double(x) + 0.5) / Double(ow) * 2 - 1) * normalSlopeX
            let ny = ((Double(y) + 0.5) / Double(oh) * 2 - 1) * normalSlopeY
            let cosine = max(0, (nx * lightX + ny * lightY + lightZ) / (sqrt(nx * nx + ny * ny + 1) * lightNorm))
            ambient[y * ow + x] = Float(albedo * ambientLux / (Double.pi * referenceWhiteCdM2) * (diffuseFraction + (1 - diffuseFraction) * cosine))
        } }
        return Plan(width: width, height: height, outputWidth: ow, outputHeight: oh,
                    gain: emissionGain / (widthFraction * verticalFill) * mass, scatterFraction: scatterFraction,
                    xCount: xCount, xmap: xmap, yCount: yCount, ymap: ymap, growthLevels: levels, gCount: gCount, gmap: gmap,
                    colsum: colsum, kernels: kernels, ambient: ambient, growth: growth)
    }
}
