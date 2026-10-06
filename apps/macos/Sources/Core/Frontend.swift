// Glue for the shared frontend core (frontend/include/replaynes/frontend.h, C API rnf_*): string
// and list conversion, and copy-on-write boxes that give the core's opaque handles Swift value
// semantics. The logic itself lives in the core; the Swift types around it are thin wrappers.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Takes ownership of a heap string returned by the core (nil -> nil).
@inline(__always)
func rnfTake(_ p: UnsafeMutablePointer<CChar>?) -> String? {
    guard let p else { return nil }
    defer { rnf_string_free(p) }
    return String(cString: p)
}

/// Takes ownership of a heap string returned by the core ("" when nil).
@inline(__always)
func rnfString(_ p: UnsafeMutablePointer<CChar>?) -> String { rnfTake(p) ?? "" }

/// (a, b) pairs of an rnf_list, which is freed.
func rnfPairs(_ l: OpaquePointer?) -> [(String, String)] {
    guard let l else { return [] }
    defer { rnf_list_free(l) }
    return (0..<rnf_list_count(l)).map { (String(cString: rnf_list_a(l, $0)), String(cString: rnf_list_b(l, $0))) }
}

/// (a, value) entries of an rnf_list, which is freed.
func rnfValues(_ l: OpaquePointer?) -> [(String, Int64)] {
    guard let l else { return [] }
    defer { rnf_list_free(l) }
    return (0..<rnf_list_count(l)).map { (String(cString: rnf_list_a(l, $0)), rnf_list_value(l, $0)) }
}

/// Calls body with NUL-terminated copies of strings (valid for the call only).
func withCStrings<R>(_ strings: [String], _ body: ([UnsafePointer<CChar>?]) -> R) -> R {
    let copies = strings.map { strdup($0) }
    defer { copies.forEach { free($0) } }
    return body(copies.map { UnsafePointer($0) })
}

/// Calls body with a C view (rnf_binding array) of a binding table.
func withBindings<R>(_ bindings: [(input: String, action: String)], _ body: (UnsafePointer<rnf_binding>?, Int) -> R) -> R {
    withCStrings(bindings.flatMap { [$0.input, $0.action] }) { c in
        var v: [rnf_binding] = []
        v.reserveCapacity(bindings.count)
        for i in 0..<bindings.count { v.append(rnf_binding(input: c[2 * i], action: c[2 * i + 1])) }
        return v.withUnsafeBufferPointer { body($0.baseAddress, $0.count) }
    }
}

/// Owner of one core handle. Value types hold it and copy it before mutating when shared, so they
/// keep the value semantics of the structs they replaced.
final class RNFHandle {
    let ptr: OpaquePointer
    private let free: (OpaquePointer) -> Void
    private let clone: (OpaquePointer) -> OpaquePointer?

    init(_ ptr: OpaquePointer?, free: @escaping (OpaquePointer) -> Void, clone: @escaping (OpaquePointer) -> OpaquePointer?) {
        guard let ptr else { fatalError("frontend core: out of memory") }
        self.ptr = ptr
        self.free = free
        self.clone = clone
    }

    deinit { free(ptr) }

    func copy() -> RNFHandle { RNFHandle(clone(ptr), free: free, clone: clone) }
}

extension RNFHandle {
    /// The handle to mutate: `box` itself if uniquely referenced, a fresh copy otherwise.
    static func unique(_ box: inout RNFHandle) -> OpaquePointer {
        if !isKnownUniquelyReferenced(&box) { box = box.copy() }
        return box.ptr
    }
}
