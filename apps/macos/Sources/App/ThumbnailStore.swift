// Library thumbnails: the picture of the last session of each project and each ROM (by SHA-256),
// small PNGs (256x224) in <session folder>/Thumbnails. Written off the main thread when the app
// goes to the background, quits, closes a game or shows the library; read (and cached) by the
// library screen. Only where sessions are persisted (not in scripted runs without --session-root).
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import CryptoKit
import ImageIO
import UniformTypeIdentifiers

enum ThumbnailStore {
    /// Set once at launch (AppModel.setupSessionPersistence).
    static var dir: URL?
    private static let cache = NSCache<NSString, CGImage>()

    static func url(project path: String) -> URL? {
        let h = SHA256.hash(data: Data(path.utf8)).prefix(12).map { String(format: "%02x", $0) }.joined()
        return dir?.appendingPathComponent("p-\(h).png")
    }

    static func url(romSHA sha: String) -> URL? {
        guard !sha.isEmpty else { return nil }
        return dir?.appendingPathComponent("r-\(sha.prefix(24)).png")
    }

    /// `pixels`: one 256x240 BGRA frame. The overscan rows are cropped.
    static func save(pixels: [UInt32], project: String?, romSHA: String?, on queue: DispatchQueue, done: @escaping () -> Void) {
        guard let dir else { return }
        let targets = [project.flatMap { url(project: $0) }, romSHA.flatMap { url(romSHA: $0) }].compactMap { $0 }
        guard !targets.isEmpty else { return }
        queue.async {
            let w = Int(RN_VIDEO_WIDTH), h = Int(RN_VIDEO_HEIGHT), crop = 8
            let data = pixels.withUnsafeBufferPointer { Data(buffer: $0) }
            guard let provider = CGDataProvider(data: data as CFData),
                  let full = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                                     space: CGColorSpaceCreateDeviceRGB(),
                                     bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                                     provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent),
                  let img = full.cropping(to: CGRect(x: 0, y: crop, width: w, height: h - 2 * crop)) else { return }
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            for u in targets {
                guard let dest = CGImageDestinationCreateWithURL(u as CFURL, UTType.png.identifier as CFString, 1, nil) else { continue }
                CGImageDestinationAddImage(dest, img, nil)
                if CGImageDestinationFinalize(dest) { cache.removeObject(forKey: u.path as NSString) }
            }
            DispatchQueue.main.async(execute: done)
        }
    }

    static func image(project path: String) -> CGImage? { load(url(project: path)) }
    static func image(romSHA sha: String?) -> CGImage? { sha.flatMap { load(url(romSHA: $0)) } }

    private static func load(_ u: URL?) -> CGImage? {
        guard let u else { return nil }
        if let c = cache.object(forKey: u.path as NSString) { return c }
        guard let src = CGImageSourceCreateWithURL(u as CFURL, nil), let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else { return nil }
        cache.setObject(img, forKey: u.path as NSString)
        return img
    }
}
