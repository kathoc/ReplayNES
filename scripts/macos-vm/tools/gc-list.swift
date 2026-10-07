// Lists game controllers as macOS sees them: HID gamepads/joysticks (IOKit) and the
// GameController.framework view the app uses. Built on the host by scripts/macos-vm/controllers.sh
// and run in the guest (which has no compiler). Usage: gc-list [seconds]
import Foundation
import GameController
import IOKit.hid

let wait = Double(CommandLine.arguments.dropFirst().first ?? "") ?? 4

let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
let matches: [[String: Any]] = [
    [kIOHIDDeviceUsagePageKey: kHIDPage_GenericDesktop, kIOHIDDeviceUsageKey: kHIDUsage_GD_GamePad],
    [kIOHIDDeviceUsagePageKey: kHIDPage_GenericDesktop, kIOHIDDeviceUsageKey: kHIDUsage_GD_Joystick],
    [kIOHIDDeviceUsagePageKey: kHIDPage_GenericDesktop, kIOHIDDeviceUsageKey: kHIDUsage_GD_MultiAxisController],
]
IOHIDManagerSetDeviceMatchingMultiple(mgr, matches as CFArray)
IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
let devs = (IOHIDManagerCopyDevices(mgr) as? Set<IOHIDDevice>) ?? []
print("HID game devices: \(devs.count)")
for d in devs {
    func p(_ k: String) -> Any? { IOHIDDeviceGetProperty(d, k as CFString) }
    let vid = (p(kIOHIDVendorIDKey) as? Int).map { String(format: "%04x", $0) } ?? "?"
    let pid = (p(kIOHIDProductIDKey) as? Int).map { String(format: "%04x", $0) } ?? "?"
    print("  \(p(kIOHIDProductKey) ?? "?") vid=\(vid) pid=\(pid) transport=\(p(kIOHIDTransportKey) ?? "?")")
}

GCController.shouldMonitorBackgroundEvents = true
NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { n in
    let c = n.object as! GCController
    print("  connected: \(c.vendorName ?? "?") [\(c.productCategory)]")
}
GCController.startWirelessControllerDiscovery {}
RunLoop.main.run(until: Date().addingTimeInterval(wait))
let cs = GCController.controllers()
print("GameController.framework controllers: \(cs.count)")
for c in cs {
    print("  \(c.vendorName ?? "?") [\(c.productCategory)] extendedGamepad=\(c.extendedGamepad != nil)")
}
exit(cs.isEmpty ? 1 : 0)
