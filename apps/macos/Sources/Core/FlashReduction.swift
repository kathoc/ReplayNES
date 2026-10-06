// Swift wrapper over the engine's photosensitive flash reduction filter (rn_flash_filter_*); the
// level texts come from the shared frontend core.
// Display-side only: it processes copies of frames that are shown or exported and never touches
// the session, so recorded input and verification hashes are unaffected.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum FlashLevel: Int, CaseIterable, Identifiable {
    case off = 0, low = 1, standard = 2, high = 3
    var id: Int { rawValue }
    var label: String { rnfString(rnf_flash_level_label(cValue)) }
    var detail: String { rnfString(rnf_flash_level_detail(cValue)) }
    var cValue: rn_flash_level { rn_flash_level(rawValue: UInt32(rawValue)) }
}

/// Not thread-safe: use from one thread (the emulation thread, or one export job).
final class FlashFilter {
    private let handle: OpaquePointer
    private(set) var level: FlashLevel

    init(level: FlashLevel) {
        guard let h = rn_flash_filter_new(level.cValue) else { fatalError("rn_flash_filter_new: \(String(cString: rn_last_error()))") }
        handle = h
        self.level = level
    }
    deinit { rn_flash_filter_free(handle) }

    func setLevel(_ l: FlashLevel) {
        guard l != level else { return }
        level = l
        _ = rn_flash_filter_set_level(handle, l.cValue)
    }

    /// Call on every discontinuity (seek, load, take switch, start of rewind / scrub).
    func reset() { rn_flash_filter_reset(handle) }

    /// Filters one 256x240 BGRA frame into dst. Returns true if the output differs from the input.
    @discardableResult
    func process(_ src: UnsafePointer<UInt32>, into dst: UnsafeMutablePointer<UInt32>) -> Bool {
        var info = rn_flash_info()
        guard rn_flash_filter_process(handle, src, dst, &info) == RN_OK else {
            dst.update(from: src, count: Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT))
            return false
        }
        return info.altered != 0
    }
}
