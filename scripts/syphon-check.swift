// Headless Syphon client used by scripts/check-syphon-macos.sh: waits for the "ReplayNES" Syphon
// server, receives frames with SyphonMetalClient and checks their size and content.
//   syphon-check <expected-width> <expected-height> [pillar-width]
// Exit 0 on success. pillar-width > 0: the left/right bars of that width must be black.
// SYPHON_CHECK_PNG=<path>: also save the received frame (for eyeballing orientation/scaling).
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import Metal
import Syphon

func fail(_ m: String) -> Never { print("FAIL: \(m)"); exit(1) }
func spin(_ s: Double) { RunLoop.main.run(until: Date().addingTimeInterval(s)) }

let args = CommandLine.arguments
guard args.count >= 3, let ew = Int(args[1]), let eh = Int(args[2]) else { fail("usage: syphon-check <w> <h> [pillar]") }
let pillar = args.count > 3 ? Int(args[3]) ?? 0 : 0
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fail("no Metal") }

// 1. Discover the server (the directory is fed by distributed notifications: run the run loop).
let dir = SyphonServerDirectory.shared()
var desc: [String: any NSCoding]?
let deadline = Date().addingTimeInterval(20)
while desc == nil && Date() < deadline {
    spin(0.2)
    desc = dir.servers(matchingName: "ReplayNES", appName: nil).first
}
guard let desc else { fail("no Syphon server named ReplayNES (servers: \(dir.servers.map { $0[SyphonServerDescriptionNameKey] as? String ?? "?" }))") }
print("server: name=\(desc[SyphonServerDescriptionNameKey] as? String ?? "?") app=\(desc[SyphonServerDescriptionAppNameKey] as? String ?? "?")")

// 2. Receive frames.
let lock = NSLock()
var frameCount = 0
let client = SyphonMetalClient(serverDescription: desc, device: device, options: nil) { _ in
    lock.lock(); frameCount += 1; lock.unlock()
}
var image: MTLTexture?
let frameDeadline = Date().addingTimeInterval(10)
while image == nil && Date() < frameDeadline {
    spin(0.1)
    image = client.newFrameImage()
}
guard let image else { fail("server found but no frame received") }
lock.lock(); let before = frameCount; lock.unlock()
spin(2.0)
lock.lock(); let after = frameCount; lock.unlock()
print("frame: \(image.width)x\(image.height), frames in 2 s: \(after - before)")
guard image.width == ew, image.height == eh else { fail("expected \(ew)x\(eh)") }
guard after - before >= 30 else { fail("frame rate too low (\(after - before) in 2 s); is the game running?") }

// 3. Content: read back the latest frame.
guard let latest = client.newFrameImage() else { fail("lost frame") }
let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: latest.width, height: latest.height, mipmapped: false)
td.storageMode = .shared
guard let copy = device.makeTexture(descriptor: td), let cb = queue.makeCommandBuffer(), let blit = cb.makeBlitCommandEncoder() else { fail("readback setup") }
blit.copy(from: latest, to: copy)
blit.endEncoding()
cb.commit()
cb.waitUntilCompleted()
var px = [UInt32](repeating: 0, count: latest.width * latest.height)
copy.getBytes(&px, bytesPerRow: latest.width * 4, from: MTLRegionMake2D(0, 0, latest.width, latest.height), mipmapLevel: 0)
var nonBlack = 0, pillarNonBlack = 0
for y in 0..<latest.height {
    for x in 0..<latest.width where px[y * latest.width + x] & 0x00FF_FFFF != 0 {
        if x < pillar || x >= latest.width - pillar { pillarNonBlack += 1 } else { nonBlack += 1 }
    }
}
print("content: non-black pixels \(nonBlack), in bars \(pillarNonBlack)")
guard nonBlack > 0 else { fail("picture is entirely black") }
guard pillarNonBlack == 0 else { fail("letterbox bars are not black") }
if let out = ProcessInfo.processInfo.environment["SYPHON_CHECK_PNG"],
   let ctx = CGContext(data: &px, width: latest.width, height: latest.height, bitsPerComponent: 8, bytesPerRow: latest.width * 4,
                       space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
   let img = ctx.makeImage(), let png = NSBitmapImageRep(cgImage: img).representation(using: .png, properties: [:]) {
    try? png.write(to: URL(fileURLWithPath: out))
    print("saved \(out)")
}
client.stop()
print("OK")
