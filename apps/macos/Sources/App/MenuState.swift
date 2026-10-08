// The few values the menu bar depends on. SwiftUI rebuilds every menu item whenever an observed
// object publishes; AppModel publishes many times per second (frame counter, latency stats), which
// made the whole menu bar flicker. Menus observe only this object, which publishes when one of
// these values actually changes.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct MenuValues: Equatable {
    var hasSession = false
    var paused = true
    var recording = true
    var practicing = false
    var slowOn = false
    var takeEmpty = true
    var undoAvailable = false
    var integerScale = true
    var showLatency = false
    var streamOn = false
}

final class MenuState: ObservableObject {
    @Published private(set) var v = MenuValues()

    func set(_ new: MenuValues) {
        if new != v { v = new }
    }
}
