// Headless CRT benchmark and quality harness (no window, host GPU): renders a frame sequence of raw
// PPU output (replaynes-cli dump-ppu, or a built-in synthetic scroll) through CRTRenderer at
// several tube sizes and reports GPU time per frame (command-buffer GPU start/end), optionally per
// pass (timestamp samples), and the error of one path against another (PSNR / mean dE76 of the
// sRGB-encoded tube picture, PNGs of both and of the difference).
// Built by scripts/bench-crt-macos.sh. SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import Foundation
import ImageIO
import Metal

struct Options {
    var ppu: [String] = []
    var sizes: [(Int, Int)] = [(640, 480), (1144, 858), (1600, 1200)]
    var frames = 600, warmup = 60
    var pace = 0.0                      // Hz; 0 = back to back
    var profile = false
    var compare: String?                // "reference:fast" etc.
    var mode = "fast"
    var png: String?
    var pngFrames: [Int] = []
    var noise = true
    var settings = CRTRenderer.Settings()
    var skip: Set<String> = []
}

func parse() -> Options {
    var o = Options()
    var a = Array(CommandLine.arguments.dropFirst())
    func next() -> String { a.isEmpty ? "" : a.removeFirst() }
    while !a.isEmpty {
        let k = next()
        switch k {
        case "--ppu": o.ppu.append(next())
        case "--sizes": o.sizes = next().split(separator: ",").map { s in let p = s.split(separator: "x"); return (Int(p[0])!, Int(p[1])!) }
        case "--frames": o.frames = Int(next())!
        case "--warmup": o.warmup = Int(next())!
        case "--pace": o.pace = Double(next())!
        case "--profile": o.profile = true
        case "--compare": o.compare = next()
        case "--mode": o.mode = next()
        case "--png": o.png = next()
        case "--png-frames": o.pngFrames = next().split(separator: ",").map { Int($0)! }
        case "--no-noise": o.noise = false
        case "--no-persistence": o.settings.persistence = false
        case "--no-growth": o.settings.beamGrowth = false
        case "--no-supply": o.settings.supply = false
        case "--lines": o.settings.lines = Int(next())!
        case "--skip": o.skip = Set(next().split(separator: ",").map(String.init))
        default: FileHandle.standardError.write("unknown option \(k)\n".data(using: .utf8)!); exit(1)
        }
    }
    return o
}

struct Frame { var ordinal: UInt64; var burst: UInt32; var codes: [UInt16] }

func loadPPU(_ path: String) -> [Frame] {
    guard let d = FileManager.default.contents(atPath: path), d.count >= 16, d.prefix(6) == Data("RNPPU1".utf8) else {
        fatalError("bad ppu file \(path)")
    }
    let n = Int(d.withUnsafeBytes { $0.load(fromByteOffset: 8, as: UInt32.self) })
    let stride = 16 + 256 * 240 * 2
    return (0..<n).map { i in
        d.withUnsafeBytes { raw in
            let base = 16 + i * stride
            let ord = raw.loadUnaligned(fromByteOffset: base, as: UInt64.self)
            let b = raw.loadUnaligned(fromByteOffset: base + 8, as: UInt32.self)
            let codes = (0..<(256 * 240)).map { raw.loadUnaligned(fromByteOffset: base + 16 + $0 * 2, as: UInt16.self) }
            return Frame(ordinal: ord, burst: b, codes: codes)
        }
    }
}

/// Synthetic sequence: colour bars / ramps / emphasis / checkerboard, scrolling one pixel a frame.
func synthetic(_ n: Int) -> [Frame] {
    let bars: [UInt16] = [0x30, 0x28, 0x2C, 0x2A, 0x24, 0x16, 0x12, 0x0F]
    return (0..<n).map { f in
        var c = [UInt16](repeating: 0, count: 256 * 240)
        for y in 0..<240 { for x0 in 0..<256 {
            let x = (x0 + f) & 255
            let v: Int
            if y < 100 { v = Int(bars[x >> 5]) }
            else if y < 150 { v = ((x >> 4) & 15) | ((((y - 100) / 13) & 3) << 4) }
            else if y < 200 { v = ((x >> 4) & 15) | (2 << 4) | ((((y - 150) / 7) & 7) << 6) }
            else { v = ((x >> 3) + (y >> 3)) % 2 == 0 ? 0x30 : 0x0F }
            c[y * 256 + x0] = UInt16(v)
        } }
        return Frame(ordinal: UInt64(f + 1), burst: UInt32(f % 3), codes: c)
    }
}

let opt = parse()
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fatalError("no Metal device") }
var frames: [Frame] = opt.ppu.isEmpty ? synthetic(120) : opt.ppu.flatMap { loadPPU($0) }
// Ordinals must increase through the whole (possibly concatenated) sequence.
for i in frames.indices { frames[i].ordinal = UInt64(i + 1) }
print("device \(device.name), \(frames.count) input frames, settings \(opt.settings), noise \(opt.noise)")

func makeRenderer(_ mode: String, _ w: Int, _ h: Int) -> CRTRenderer {
    let r = try! CRTRenderer(device: device, targetPixelFormat: nil)
    applyMode(r, mode)
    r.noiseEnabled = opt.noise
    r.benchSkipPasses = opt.skip
    r.configure(settings: opt.settings, outputWidth: w, outputHeight: h, synchronous: true)
    return r
}

func applyMode(_ r: CRTRenderer, _ mode: String) {
    switch mode {
    case "fast": r.quality = .fast
    case "reference": r.quality = .reference
    case "direct": r.quality = .reference; r.useFastFFT = false; r.useFastScatter = false
    default: fatalError("unknown mode \(mode)")
    }
}

func target(_ w: Int, _ h: Int, _ fmt: MTLPixelFormat = .rgba8Unorm, shared: Bool = false) -> MTLTexture {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: fmt, width: w, height: h, mipmapped: false)
    d.usage = [.shaderWrite, .shaderRead]
    d.storageMode = shared ? .shared : .private
    return device.makeTexture(descriptor: d)!
}

func encodeFrame(_ r: CRTRenderer, _ f: Frame, _ cb: MTLCommandBuffer, _ tex: MTLTexture) {
    f.codes.withUnsafeBufferPointer { _ = r.encode(.codes($0.baseAddress!, burstPhase: f.burst), ordinal: f.ordinal, into: cb) }
    r.encodeShow(into: tex, cb: cb, dst: CGRect(x: 0, y: 0, width: tex.width, height: tex.height), cropFraction: 0)
}

func stats(_ v: [Double]) -> String {
    let s = v.sorted()
    func p(_ q: Double) -> Double { s[min(s.count - 1, Int(q * Double(s.count)))] }
    return String(format: "mean %.3f  p50 %.3f  p90 %.3f  min %.3f ms", v.reduce(0, +) / Double(v.count), p(0.5), p(0.9), s.first ?? 0)
}

func sleepUntil(_ t: Double) {
    let now = CFAbsoluteTimeGetCurrent()
    if t > now { usleep(useconds_t((t - now) * 1e6)) }
}

// MARK: timing

func bench(_ w: Int, _ h: Int) {
    let r = makeRenderer(opt.mode, w, h)
    let tex = target(w, h)
    var ms: [Double] = []
    let period = opt.pace > 0 ? 1 / opt.pace : 0
    var next = CFAbsoluteTimeGetCurrent()
    // pace 0: back to back (each frame waited for, the next encoded at once: high clocks);
    // pace > 0: one frame per period like the live display (the GPU's clock management decides).
    for i in 0..<(opt.warmup + opt.frames) {
        let cb = queue.makeCommandBuffer()!
        encodeFrame(r, frames[i % frames.count], cb, tex)
        cb.commit()
        cb.waitUntilCompleted()
        if i >= opt.warmup { ms.append((cb.gpuEndTime - cb.gpuStartTime) * 1000) }
        if period > 0 { next += period; sleepUntil(next) }
    }
    print(String(format: "%5dx%-5d %@ %@ GPU/frame: ", w, h, opt.mode, opt.pace > 0 ? "paced \(Int(opt.pace)) Hz" : "back-to-back") + stats(ms))
    if opt.profile, let prof = CRTPassProfiler(device: device) {
        r.profiler = prof
        let (cpu0, gpu0) = device.sampleTimestamps()
        var samples: [String: [Double]] = [:], order: [String] = []
        let n = min(opt.frames, 300)
        next = CFAbsoluteTimeGetCurrent()
        for i in 0..<n {
            let cb = queue.makeCommandBuffer()!
            encodeFrame(r, frames[i % frames.count], cb, tex)
            cb.commit()
            cb.waitUntilCompleted()
            var frame: [String: Double] = [:]
            for (name, ticks) in prof.results() {
                if samples[name] == nil { order.append(name); samples[name] = [] }
                frame[name, default: 0] += Double(ticks)
            }
            for (k, v) in frame { samples[k]!.append(v) }
            if period > 0 { next += period; sleepUntil(next) }
        }
        let (cpu1, gpu1) = device.sampleTimestamps()
        // sampleTimestamps' CPU side is in nanoseconds (as is the GPU side on Apple GPUs today).
        let nsPerTick = Double(cpu1 - cpu0) / Double(max(1, gpu1 - gpu0))
        var totMed = 0.0, totMean = 0.0
        print("    pass                    median     mean      min  (ms; each pass in its own encoder)")
        for name in order {
            let v = samples[name]!.map { $0 * nsPerTick / 1e6 }.sorted()
            let med = v[v.count / 2], mean = v.reduce(0, +) / Double(v.count)
            totMed += med; totMean += mean
            print("    " + name.padding(toLength: 20, withPad: " ", startingAt: 0) + String(format: " %8.3f %8.3f %8.3f", med, mean, v.first!))
        }
        print("    total               " + String(format: " %8.3f %8.3f", totMed, totMean))
        r.profiler = nil
    }
}

// MARK: quality

func readBack(_ tex: MTLTexture) -> [Float] {
    let w = tex.width, h = tex.height
    let shared = target(w, h, .rgba32Float, shared: true)
    let cb = queue.makeCommandBuffer()!
    let b = cb.makeBlitCommandEncoder()!
    b.copy(from: tex, to: shared)
    b.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    var out = [Float](repeating: 0, count: w * h * 4)
    shared.getBytes(&out, bytesPerRow: w * 16, from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
    return out
}

func lab(_ r: Float, _ g: Float, _ b: Float) -> SIMD3<Double> {
    func lin(_ c: Float) -> Double { let v = Double(c); return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4) }
    let R = lin(r), G = lin(g), B = lin(b)
    let X = (0.4124 * R + 0.3576 * G + 0.1805 * B) / 0.95047, Y = 0.2126 * R + 0.7152 * G + 0.0722 * B
    let Z = (0.0193 * R + 0.1192 * G + 0.9505 * B) / 1.08883
    func f(_ t: Double) -> Double { t > 216.0 / 24389 ? cbrt(t) : (24389.0 / 27 * t + 16) / 116 }
    return SIMD3(116 * f(Y) - 16, 500 * (f(X) - f(Y)), 200 * (f(Y) - f(Z)))
}

func writePNG(_ px: [Float], _ w: Int, _ h: Int, _ path: String, scale: Float = 1) {
    var bytes = [UInt8](repeating: 255, count: w * h * 4)
    for i in 0..<(w * h) { for c in 0..<3 { bytes[i * 4 + c] = UInt8(max(0, min(255, (px[i * 4 + c] * scale * 255).rounded()))) } }
    let ctx = CGContext(data: &bytes, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, "public.png" as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, ctx.makeImage()!, nil)
    CGImageDestinationFinalize(dest)
}

func compare(_ spec: String, _ w: Int, _ h: Int) {
    let modes = spec.split(separator: ":").map(String.init)
    let ra = makeRenderer(modes[0], w, h), rb = makeRenderer(modes[1], w, h)
    let ta = target(w, h, .rgba32Float), tb = target(w, h, .rgba32Float)
    var worst = Double.infinity, psnrSum = 0.0, deSum = 0.0, deMax = 0.0, maxAbs: Float = 0, count = 0
    let n = min(opt.frames, frames.count)
    for i in 0..<n {
        let cb = queue.makeCommandBuffer()!
        encodeFrame(ra, frames[i], cb, ta)
        encodeFrame(rb, frames[i], cb, tb)
        cb.commit(); cb.waitUntilCompleted()
        // Every frame feeds the temporal state; measure every 10th (and the PNG frames).
        guard i % 10 == 9 || opt.pngFrames.contains(i) else { continue }
        let a = readBack(ta), b = readBack(tb)
        var se = 0.0, de = 0.0
        for p in 0..<(w * h) {
            for c in 0..<3 { let d = a[p * 4 + c] - b[p * 4 + c]; se += Double(d * d); maxAbs = max(maxAbs, abs(d)) }
            if p % 7 == 0 {
                let e = lab(a[p * 4], a[p * 4 + 1], a[p * 4 + 2]) - lab(b[p * 4], b[p * 4 + 1], b[p * 4 + 2])
                let d = (e * e).sum().squareRoot()
                de += d; deMax = max(deMax, d)
            }
        }
        let mse = se / Double(w * h * 3)
        let psnr = mse > 0 ? 10 * log10(1 / mse) : 200
        worst = min(worst, psnr); psnrSum += psnr; deSum += de / Double((w * h + 6) / 7); count += 1
        if let dir = opt.png, opt.pngFrames.contains(i) {
            let tag = "\(dir)/\(w)x\(h)_f\(i)"
            writePNG(a, w, h, "\(tag)_\(modes[0]).png")
            writePNG(b, w, h, "\(tag)_\(modes[1]).png")
            writePNG(zip(a, b).map { abs($0 - $1) }, w, h, "\(tag)_diff_x10.png", scale: 10)
        }
    }
    print(String(format: "%5dx%-5d %@ vs %@: PSNR mean %.2f dB, worst %.2f dB; dE76 mean %.4f, max %.3f; max |diff| %.4f (%d frames)",
                 w, h, modes[0], modes[1], psnrSum / Double(max(1, count)), worst, deSum / Double(max(1, count)), deMax, maxAbs, count))
}

for (w, h) in opt.sizes {
    if let c = opt.compare { compare(c, w, h) } else { bench(w, h) }
}
