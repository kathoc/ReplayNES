// Action names, Japanese labels, default bindings and physical-id display names.
// The binding table itself lives in the engine (rn_input JSON); this file only describes it.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct InputAction: Identifiable, Hashable {
    let id: String      // engine action name, e.g. "p1.a", "hk.pause"
    let label: String
    let group: Group

    enum Group: String, CaseIterable {
        case player1 = "プレイヤー1"
        case player2 = "プレイヤー2"
        case hotkey = "ホットキー"
    }
}

enum InputCatalog {
    static let gameActions: [InputAction] = {
        var out: [InputAction] = []
        for (p, g) in [("p1", InputAction.Group.player1), ("p2", .player2)] {
            out += [
                InputAction(id: "\(p).up", label: "↑ 上", group: g),
                InputAction(id: "\(p).down", label: "↓ 下", group: g),
                InputAction(id: "\(p).left", label: "← 左", group: g),
                InputAction(id: "\(p).right", label: "→ 右", group: g),
                InputAction(id: "\(p).a", label: "A", group: g),
                InputAction(id: "\(p).b", label: "B", group: g),
                InputAction(id: "\(p).select", label: "SELECT", group: g),
                InputAction(id: "\(p).start", label: "START", group: g),
                InputAction(id: "\(p).turbo_a", label: "連射 A", group: g),
                InputAction(id: "\(p).turbo_b", label: "連射 B", group: g),
            ]
        }
        return out
    }()

    static let hotkeyActions: [InputAction] = [
        InputAction(id: "hk.pause", label: "一時停止 / 再開", group: .hotkey),
        InputAction(id: "hk.frame_advance", label: "コマ送り", group: .hotkey),
        InputAction(id: "hk.step_back", label: "1コマ戻る", group: .hotkey),
        InputAction(id: "hk.rewind", label: "巻き戻し (押している間)", group: .hotkey),
        InputAction(id: "hk.fast_forward", label: "早送り (押している間)", group: .hotkey),
        InputAction(id: "hk.slow", label: "スロー切替 (等速→1/2→1/4)", group: .hotkey),
        InputAction(id: "hk.bookmark", label: "ブックマーク追加", group: .hotkey),
        InputAction(id: "hk.soft_reset", label: "ソフトリセット", group: .hotkey),
        InputAction(id: "hk.power_cycle", label: "電源再投入", group: .hotkey),
        InputAction(id: "hk.toggle_mode", label: "録画 / 再生モード切替", group: .hotkey),
        InputAction(id: "hk.save", label: "保存", group: .hotkey),
        InputAction(id: "hk.undo_take", label: "前の試行へ戻す", group: .hotkey),
    ]

    static var allActions: [InputAction] { gameActions + hotkeyActions }

    /// Default keyboard + controller layout (macOS virtual key codes).
    static let defaultBindings: [(String, String)] = [
        ("kb:126", "p1.up"), ("kb:125", "p1.down"), ("kb:123", "p1.left"), ("kb:124", "p1.right"),
        ("kb:7", "p1.a"), ("kb:6", "p1.b"),               // X = A, Z = B
        ("kb:1", "p1.turbo_a"), ("kb:0", "p1.turbo_b"),   // S = turbo A, A = turbo B
        ("kb:36", "p1.start"), ("kb:60", "p1.select"),    // Return = START, right Shift = SELECT
        ("kb:42", "p1.select"),                           // \ = SELECT (laptops)
        ("kb:49", "hk.pause"),                            // Space
        ("kb:47", "hk.frame_advance"),                    // .
        ("kb:43", "hk.step_back"),                        // ,
        ("kb:51", "hk.rewind"),                           // Delete (Backspace)
        ("kb:48", "hk.fast_forward"),                     // Tab
        ("kb:37", "hk.slow"),                             // L
        ("kb:11", "hk.bookmark"),                         // B
        // Controller slot 0 -> P1, slot 1 -> P2 (Nintendo-style: east = A, south = B)
        ("gc0:dpad.up", "p1.up"), ("gc0:dpad.down", "p1.down"), ("gc0:dpad.left", "p1.left"), ("gc0:dpad.right", "p1.right"),
        ("gc0:lstick.up", "p1.up"), ("gc0:lstick.down", "p1.down"), ("gc0:lstick.left", "p1.left"), ("gc0:lstick.right", "p1.right"),
        ("gc0:buttonB", "p1.a"), ("gc0:buttonA", "p1.b"), ("gc0:buttonY", "p1.turbo_a"), ("gc0:buttonX", "p1.turbo_b"),
        ("gc0:menu", "p1.start"), ("gc0:options", "p1.select"),
        ("gc0:leftShoulder", "hk.rewind"), ("gc0:rightShoulder", "hk.fast_forward"),
        ("gc0:leftTrigger", "hk.step_back"), ("gc0:rightTrigger", "hk.frame_advance"),
        ("gc1:dpad.up", "p2.up"), ("gc1:dpad.down", "p2.down"), ("gc1:dpad.left", "p2.left"), ("gc1:dpad.right", "p2.right"),
        ("gc1:lstick.up", "p2.up"), ("gc1:lstick.down", "p2.down"), ("gc1:lstick.left", "p2.left"), ("gc1:lstick.right", "p2.right"),
        ("gc1:buttonB", "p2.a"), ("gc1:buttonA", "p2.b"), ("gc1:buttonY", "p2.turbo_a"), ("gc1:buttonX", "p2.turbo_b"),
        ("gc1:menu", "p2.start"), ("gc1:options", "p2.select"),
    ]

    static func defaultConfigJSON() -> String {
        let b = defaultBindings.map { ["input": $0.0, "action": $0.1] }
        let obj: [String: Any] = ["version": 1, "bindings": b, "turbo": ["period": 4, "duty": 2],
                                  "socd": "neutral", "analogThreshold": 0.5]
        let data = try! JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys])
        return String(decoding: data, as: UTF8.self)
    }

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
        guard let obj = try? JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any] else { return nil }
        let b = (obj["bindings"] as? [[String: Any]] ?? []).compactMap { e -> (String, String)? in
            guard let i = e["input"] as? String, let a = e["action"] as? String else { return nil }
            return (i, a)
        }
        let t = obj["turbo"] as? [String: Any] ?? [:]
        return Config(bindings: b.map { (input: $0.0, action: $0.1) },
                      turboPeriod: (t["period"] as? NSNumber)?.intValue ?? 2,
                      turboDuty: (t["duty"] as? NSNumber)?.intValue ?? 1,
                      socd: obj["socd"] as? String ?? "neutral",
                      analogThreshold: (obj["analogThreshold"] as? NSNumber)?.doubleValue ?? 0.5)
    }

    // MARK: display names
    private static let keyNames: [UInt16: String] = [
        0: "A", 1: "S", 2: "D", 3: "F", 4: "H", 5: "G", 6: "Z", 7: "X", 8: "C", 9: "V", 11: "B", 12: "Q", 13: "W", 14: "E",
        15: "R", 16: "Y", 17: "T", 18: "1", 19: "2", 20: "3", 21: "4", 22: "6", 23: "5", 24: "=", 25: "9", 26: "7", 27: "-",
        28: "8", 29: "0", 30: "]", 31: "O", 32: "U", 33: "[", 34: "I", 35: "P", 36: "Return", 37: "L", 38: "J", 39: "'",
        40: "K", 41: ";", 42: "\\", 43: ",", 44: "/", 45: "N", 46: "M", 47: ".", 48: "Tab", 49: "Space", 50: "`",
        51: "Delete", 53: "Esc", 54: "右⌘", 55: "⌘", 56: "左Shift", 57: "Caps", 58: "左Option", 59: "左Control",
        60: "右Shift", 61: "右Option", 62: "右Control", 63: "fn", 65: "テンキー .", 67: "テンキー *", 69: "テンキー +",
        71: "Clear", 75: "テンキー /", 76: "Enter", 78: "テンキー -", 81: "テンキー =", 82: "テンキー 0", 83: "テンキー 1",
        84: "テンキー 2", 85: "テンキー 3", 86: "テンキー 4", 87: "テンキー 5", 88: "テンキー 6", 89: "テンキー 7",
        91: "テンキー 8", 92: "テンキー 9", 96: "F5", 97: "F6", 98: "F7", 99: "F3", 100: "F8", 101: "F9", 103: "F11",
        109: "F10", 111: "F12", 115: "Home", 116: "PageUp", 117: "⌦", 118: "F4", 119: "End", 120: "F2", 121: "PageDown",
        122: "F1", 123: "←", 124: "→", 125: "↓", 126: "↑", 102: "英数", 104: "かな",
    ]

    static func displayName(_ physicalID: String) -> String {
        if physicalID.hasPrefix("kb:"), let code = UInt16(physicalID.dropFirst(3)) {
            return "キー " + (keyNames[code] ?? "#\(code)")
        }
        if physicalID.hasPrefix("gc"), let colon = physicalID.firstIndex(of: ":") {
            let slot = Int(physicalID[physicalID.index(physicalID.startIndex, offsetBy: 2)..<colon]) ?? 0
            let name = String(physicalID[physicalID.index(after: colon)...])
            let pretty: [String: String] = [
                "buttonA": "A(下)", "buttonB": "B(右)", "buttonX": "X(左)", "buttonY": "Y(上)",
                "dpad.up": "十字↑", "dpad.down": "十字↓", "dpad.left": "十字←", "dpad.right": "十字→",
                "lstick.up": "左スティック↑", "lstick.down": "左スティック↓", "lstick.left": "左スティック←", "lstick.right": "左スティック→",
                "rstick.up": "右スティック↑", "rstick.down": "右スティック↓", "rstick.left": "右スティック←", "rstick.right": "右スティック→",
                "leftShoulder": "L1", "rightShoulder": "R1", "leftTrigger": "L2", "rightTrigger": "R2",
                "menu": "Menu", "options": "Options", "home": "Home", "leftThumb": "L3", "rightThumb": "R3",
            ]
            return "パッド\(slot + 1) " + (pretty[name] ?? name)
        }
        return physicalID
    }
}
