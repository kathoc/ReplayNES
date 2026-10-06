// Action names, localized labels, default bindings, layout migrations and physical-id display
// names. The binding table itself lives in the engine (rn_input JSON); the catalog and its rules
// live in the shared frontend core (frontend/src/input.cpp); this file adapts them to Swift.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct InputAction: Identifiable, Hashable {
    let id: String      // engine action name, e.g. "p1.a", "hk.pause"
    let label: String
    let group: Group

    enum Group: String, CaseIterable {
        case player1, player2, hotkey

        var title: String { rnfString(rnf_input_group_title(cValue)) }

        init(_ c: rnf_action_group) {
            switch c {
            case RNF_GROUP_PLAYER1: self = .player1
            case RNF_GROUP_PLAYER2: self = .player2
            default: self = .hotkey
            }
        }

        var cValue: rnf_action_group {
            switch self {
            case .player1: return RNF_GROUP_PLAYER1
            case .player2: return RNF_GROUP_PLAYER2
            case .hotkey: return RNF_GROUP_HOTKEY
            }
        }
    }
}

enum InputCatalog {
    /// Keyboard ids are macOS virtual key codes ("kb:<kVK>").
    static let keyboardScheme = RNF_KEYBOARD_MACOS

    private static let catalog: [InputAction] = (0..<rnf_input_action_count()).compactMap { i in
        var a = rnf_action_info()
        guard rnf_input_action_get(i, &a) != 0 else { return nil }
        let id = String(cString: a.id)
        return InputAction(id: id, label: rnfString(rnf_input_action_label(id)), group: .init(a.group))
    }

    static let gameActions: [InputAction] = catalog.filter { $0.group != .hotkey }
    static let hotkeyActions: [InputAction] = catalog.filter { $0.group == .hotkey }
    static var allActions: [InputAction] { gameActions + hotkeyActions }

    /// Default keyboard + controller layout (macOS virtual key codes; face buttons by position).
    static let defaultBindings: [(String, String)] = (0..<rnf_input_default_binding_count(keyboardScheme)).compactMap { i in
        var b = rnf_binding()
        guard rnf_input_default_binding_get(keyboardScheme, i, &b) != 0 else { return nil }
        return (String(cString: b.input), String(cString: b.action))
    }

    /// Controller hotkeys of layout 1 (ReplayNES <= 0.1.x defaults).
    static let legacyControllerHotkeys: [(String, String)] = rnfPairs(rnf_input_legacy_controller_hotkeys())
    /// Controller hotkeys of layout 2 (0.2.0+).
    static let controllerHotkeys: [(String, String)] = rnfPairs(rnf_input_controller_hotkeys())
    /// 3: face buttons are positional ids ("face.east"...) instead of GameController names.
    static let controllerLayoutVersion = Int(RNF_CONTROLLER_LAYOUT_VERSION)

    /// Face-button bindings of layout 2 (GameController names) for slot 0 / 1.
    static func legacyFaceDefaults(slot: Int) -> [(String, String)] { rnfPairs(rnf_input_legacy_face_defaults(Int32(slot))) }

    static let legacyFaceNames: Set<String> = ["buttonA", "buttonB", "buttonX", "buttonY"]

    static func isLegacyFace(_ input: String, slot: Int) -> Bool { rnf_input_is_legacy_face(input, Int32(slot)) != 0 }

    typealias Plan = (unbind: [(String, String)], bind: [(String, String)])

    private static func plan(_ c: Config, _ fn: (UnsafePointer<rnf_binding>?, Int, UnsafeMutablePointer<OpaquePointer?>,
                                                 UnsafeMutablePointer<OpaquePointer?>) -> Void) -> Plan {
        withBindings(c.bindings) { b, n in
            var u: OpaquePointer?, a: OpaquePointer?
            fn(b, n, &u, &a)
            return (rnfPairs(u), rnfPairs(a))
        }
    }

    /// Layout 2 -> 3, step 1 (on load): slots whose face buttons still have the untouched layout-2
    /// defaults get the new positional defaults. Customised slots keep their bindings.
    static func faceLayoutMigration(_ c: Config) -> Plan { plan(c) { rnf_input_face_layout_migration($0, $1, $2, $3) } }

    /// Layout 3 -> 4: pad 1's triggers, if still the layout-3 defaults (R2 rewind, L2 fast-forward),
    /// swap to L2 rewind / R2 fast-forward. Customised triggers are kept.
    static func triggerSwapMigration(_ c: Config) -> Plan { plan(c) { rnf_input_trigger_swap_migration($0, $1, $2, $3) } }

    /// Layout 2 -> 3, step 2 (when a controller attaches to `slot`): customised bindings that still
    /// use GameController names move to the position that name has on THAT controller.
    static func legacyFaceTranslation(_ c: Config, slot: Int, positions: [String: FacePosition]) -> Plan {
        let entries = Array(positions)
        return withCStrings(entries.map(\.key)) { names in
            let pos = entries.map { Int32($0.value.cValue.rawValue) }
            return plan(c) { b, n, u, a in
                rnf_input_legacy_face_translation(b, n, Int32(slot), names, pos, pos.count, u, a)
            }
        }
    }

    /// "Reset to Defaults" for one controller: every binding of `gc<slot>:` replaced by the defaults.
    static func controllerResetPlan(_ c: Config, slot: Int) -> Plan {
        plan(c) { rnf_input_controller_reset_plan($0, $1, Int32(slot), $2, $3) }
    }

    /// One-time upgrade of saved bindings to the 0.2.0 controller layout (untouched old hotkeys only).
    static func controllerLayoutMigration(_ c: Config) -> Plan {
        plan(c) { rnf_input_controller_layout_migration($0, $1, $2, $3) }
    }

    /// Frame-step direction for a physical id while paused (-1 = back, +1 = forward).
    static func pausedStepDirections(_ c: Config) -> [String: Int] {
        let l = withBindings(c.bindings) { b, n in rnfValues(rnf_input_paused_step_directions(b, n)) }
        var out: [String: Int] = [:]
        for (input, dir) in l { out[input] = Int(dir) }
        return out
    }

    static func defaultConfigJSON() -> String { rnfString(rnf_input_default_config_json(keyboardScheme)) }

    struct Config: Equatable {
        var bindings: [(input: String, action: String)]
        var turboPeriod: Int
        var turboDuty: Int
        var socd: String
        var analogThreshold: Double

        static func == (a: Config, b: Config) -> Bool {
            a.bindings.map { $0.input + "→" + $0.action } == b.bindings.map { $0.input + "→" + $0.action }
                && a.turboPeriod == b.turboPeriod && a.turboDuty == b.turboDuty && a.socd == b.socd && a.analogThreshold == b.analogThreshold
        }

        func inputs(for action: String) -> [String] { bindings.filter { $0.action == action }.map(\.input) }
    }

    static func parse(_ json: String) -> Config? {
        var c: OpaquePointer?
        guard rnf_input_config_parse(json, &c) == RN_OK, let c else { return nil }
        defer { rnf_input_config_free(c) }
        let bindings: [(input: String, action: String)] = (0..<rnf_input_config_binding_count(c)).compactMap { i in
            var b = rnf_binding()
            guard rnf_input_config_binding_get(c, i, &b) != 0 else { return nil }
            return (input: String(cString: b.input), action: String(cString: b.action))
        }
        return Config(bindings: bindings, turboPeriod: Int(rnf_input_config_turbo_period(c)),
                      turboDuty: Int(rnf_input_config_turbo_duty(c)), socd: String(cString: rnf_input_config_socd(c)),
                      analogThreshold: rnf_input_config_analog_threshold(c))
    }

    // MARK: display names

    /// `controllers`: connected controllers, used to add the printed label of face buttons
    /// ("Pad 1 Right Button(A)").
    static func displayName(_ physicalID: String, controllers: [ControllerInfo]) -> String {
        var slot: Int32 = 0
        var label: String?
        if rnf_input_controller_slot(physicalID, &slot) != 0, let info = controllers.first(where: { $0.slot == Int(slot) }),
           let colon = physicalID.firstIndex(of: ":") {
            label = info.label(String(physicalID[physicalID.index(after: colon)...]))
        }
        return rnfString(rnf_input_display_name(keyboardScheme, physicalID, label))
    }

    static func displayName(_ physicalID: String) -> String {
        rnfString(rnf_input_display_name(keyboardScheme, physicalID, nil))
    }
}
