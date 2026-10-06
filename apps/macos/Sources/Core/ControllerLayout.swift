// Controller families, positional face-button ids and the geometry of the settings diagram.
//
// Face buttons are bound by POSITION ("gc<slot>:face.south/east/west/north"), never by label:
// GameController.framework names Xbox / PlayStation / MFi face buttons by position (buttonA =
// south, the diamond documented in GCExtendedGamepad.h) but Nintendo controllers by their printed
// label (Switch Pro / Joy-Con pair: buttonA = the "A" button = east; SDL's SDL_mfijoystick.m
// handles the same quirk via `has_nintendo_buttons`). InputManager converts with
// `GCFaceMapping` so NES A is always the east button and NES B the south button - Nintendo's
// own A/B and the usual emulator convention for Xbox / PlayStation pads.
// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import Foundation

enum FacePosition: String, CaseIterable {
    case south, east, west, north
    /// Physical element name (the part of the id after "gc<slot>:").
    var element: String { "face." + rawValue }
}

enum ControllerFamily: String, CaseIterable, Identifiable {
    case nintendo, xbox, playStation, generic
    var id: String { rawValue }

    var title: String {
        switch self {
        case .nintendo: return String(localized: "Nintendo (Pro Controller / Joy-Con)")
        case .xbox: return "Xbox"
        case .playStation: return "PlayStation"
        case .generic: return String(localized: "Other")
        }
    }

    /// From GCDevice.productCategory (e.g. "Switch Pro Controller", "Nintendo Switch Joy-Con (L/R)",
    /// "Xbox One", "DualSense", "DualShock 4") and vendorName as a fallback.
    static func from(productCategory: String, vendorName: String? = nil) -> ControllerFamily {
        let s = (productCategory + " " + (vendorName ?? "")).lowercased()
        if s.contains("switch") || s.contains("joy-con") || s.contains("nintendo") || s.contains("pro controller") { return .nintendo }
        if s.contains("dualsense") || s.contains("dualshock") || s.contains("playstation") { return .playStation }
        if s.contains("xbox") { return .xbox }
        return .generic
    }

    /// The printed label of a physical element on this family (fallback when the device does not
    /// report one).
    func label(_ element: String) -> String {
        switch (self, element) {
        case (.nintendo, "face.south"): return "B"
        case (.nintendo, "face.east"): return "A"
        case (.nintendo, "face.west"): return "Y"
        case (.nintendo, "face.north"): return "X"
        case (.playStation, "face.south"): return "✕"
        case (.playStation, "face.east"): return "○"
        case (.playStation, "face.west"): return "□"
        case (.playStation, "face.north"): return "△"
        case (_, "face.south"): return "A"
        case (_, "face.east"): return "B"
        case (_, "face.west"): return "X"
        case (_, "face.north"): return "Y"
        case (.nintendo, "leftShoulder"): return "L"
        case (.nintendo, "rightShoulder"): return "R"
        case (.nintendo, "leftTrigger"): return "ZL"
        case (.nintendo, "rightTrigger"): return "ZR"
        case (.nintendo, "menu"): return "+"
        case (.nintendo, "options"): return "−"
        case (.nintendo, "home"): return "HOME"
        case (.xbox, "leftShoulder"): return "LB"
        case (.xbox, "rightShoulder"): return "RB"
        case (.xbox, "leftTrigger"): return "LT"
        case (.xbox, "rightTrigger"): return "RT"
        case (.xbox, "menu"): return "≡"
        case (.xbox, "options"): return "View"
        case (.xbox, "home"): return "Xbox"
        case (.playStation, "menu"): return "OPTIONS"
        case (.playStation, "options"): return "CREATE"
        case (.playStation, "home"): return "PS"
        case (_, "leftShoulder"): return "L1"
        case (_, "rightShoulder"): return "R1"
        case (_, "leftTrigger"): return "L2"
        case (_, "rightTrigger"): return "R2"
        case (_, "menu"): return "Menu"
        case (_, "options"): return "Options"
        case (_, "home"): return "Home"
        case (.xbox, "leftThumb"): return "LS"
        case (.xbox, "rightThumb"): return "RS"
        case (_, "leftThumb"): return "L3"
        case (_, "rightThumb"): return "R3"
        default: return ""
        }
    }

    /// Diagram arrangement: Nintendo / Xbox / generic put the left stick above the D-pad;
    /// PlayStation has both sticks at the bottom.
    var isSymmetric: Bool { self == .playStation }
}

/// Where GameController.framework's buttonA/B/X/Y physically are.
enum GCFaceMapping {
    static func positions(productCategory: String, vendorName: String? = nil) -> [String: FacePosition] {
        let cat = productCategory.lowercased()
        // A single Joy-Con held sideways (SDL_mfijoystick.m: A = south, B = west, X = east, Y = north).
        if cat.contains("joy-con") && (cat.hasSuffix("(l)") || cat.hasSuffix("(r)")) {
            return ["buttonA": .south, "buttonB": .west, "buttonX": .east, "buttonY": .north]
        }
        if ControllerFamily.from(productCategory: productCategory, vendorName: vendorName) == .nintendo {
            // Named by label: A is the right (east) button, B the bottom one.
            return ["buttonA": .east, "buttonB": .south, "buttonX": .north, "buttonY": .west]
        }
        // Apple's documented diamond (GCExtendedGamepad.h): A bottom, B right, X left, Y top.
        return ["buttonA": .south, "buttonB": .east, "buttonX": .west, "buttonY": .north]
    }

    /// Micro gamepads (Siri Remote and the like): A = primary, X = secondary.
    static let micro: [String: FacePosition] = ["buttonA": .south, "buttonX": .west]

    /// Text for a face-button glyph reported by the device (GCControllerElement.sfSymbolsName).
    static func label(fromSymbol name: String?) -> String? {
        guard let name else { return nil }
        let base = name.replacingOccurrences(of: ".fill", with: "").replacingOccurrences(of: ".circle", with: "")
        switch base {
        case "a", "b", "x", "y": return base.uppercased()
        case "xmark": return "✕"
        case "circle": return "○"
        case "square": return "□"
        case "triangle": return "△"
        default: return nil
        }
    }
}

/// A connected controller as the settings UI sees it.
struct ControllerInfo: Equatable, Identifiable {
    let slot: Int
    let name: String
    let productCategory: String
    let family: ControllerFamily
    /// Element -> printed label reported by the device (face buttons), overriding family labels.
    var labels: [String: String] = [:]
    var id: Int { slot }

    func label(_ element: String) -> String { labels[element] ?? family.label(element) }
}

// MARK: - diagram geometry

struct DiagramElement: Identifiable, Equatable {
    enum Kind { case face, dpad, stickDirection, stickClick, shoulder, trigger, small, home }
    enum Side { case left, right, above, below }

    let element: String   // physical element, e.g. "face.east", "dpad.up", "lstick.left"
    let kind: Kind
    let center: CGPoint
    let size: CGSize
    let badgeSide: Side
    /// "dpad", "lstick", "rstick" for directional parts (their badges may be merged).
    var group: String? = nil
    var id: String { element }

    var frame: CGRect { CGRect(x: center.x - size.width / 2, y: center.y - size.height / 2, width: size.width, height: size.height) }

    /// Point where the assignment badge is anchored (badge grows away from the element).
    var badgeAnchor: CGPoint {
        let gap: CGFloat = 4
        if kind == .stickClick {
            // Outside the stick well, below its left/right arrow badges.
            let r = ControllerDiagramLayout.stickRadius
            let dx = r + gap
            return CGPoint(x: badgeSide == .left ? center.x - dx : center.x + dx, y: center.y + r - 6)
        }
        switch badgeSide {
        case .left: return CGPoint(x: frame.minX - gap, y: center.y)
        case .right: return CGPoint(x: frame.maxX + gap, y: center.y)
        case .above: return CGPoint(x: center.x, y: frame.minY - gap)
        case .below: return CGPoint(x: center.x, y: frame.maxY + gap)
        }
    }
}

struct ControllerDiagramLayout {
    static let canvas = CGSize(width: 560, height: 262)

    let family: ControllerFamily
    let elements: [DiagramElement]
    let leftStick: (center: CGPoint, radius: CGFloat)
    let rightStick: (center: CGPoint, radius: CGFloat)
    let dpadCenter: CGPoint
    let faceCenter: CGPoint
    /// Decorative PlayStation touchpad (not bindable).
    let touchpad: CGRect?
    /// Grouped badge anchors (below the D-pad / sticks) used when a group has a standard mapping.
    let groupAnchors: [String: CGPoint]

    static let stickRadius: CGFloat = 28
    static let faceSpacing: CGFloat = 25
    static let faceRadius: CGFloat = 12
    static let dpadArm: CGFloat = 18

    init(family: ControllerFamily) {
        self.family = family
        let sym = family.isSymmetric
        let lStick = sym ? CGPoint(x: 225, y: 188) : CGPoint(x: 165, y: 120)
        let rStick = sym ? CGPoint(x: 335, y: 188) : CGPoint(x: 335, y: 182)
        let dpad = sym ? CGPoint(x: 160, y: 128) : CGPoint(x: 225, y: 182)
        let face = sym ? CGPoint(x: 400, y: 125) : CGPoint(x: 395, y: 120)
        leftStick = (lStick, Self.stickRadius)
        rightStick = (rStick, Self.stickRadius)
        dpadCenter = dpad
        faceCenter = face
        touchpad = sym ? CGRect(x: 252, y: 84, width: 56, height: 44) : nil

        var e: [DiagramElement] = []
        // Triggers / shoulders.
        e.append(DiagramElement(element: "leftTrigger", kind: .trigger, center: CGPoint(x: 150, y: 18), size: CGSize(width: 88, height: 22), badgeSide: .left))
        e.append(DiagramElement(element: "rightTrigger", kind: .trigger, center: CGPoint(x: 410, y: 18), size: CGSize(width: 88, height: 22), badgeSide: .right))
        e.append(DiagramElement(element: "leftShoulder", kind: .shoulder, center: CGPoint(x: 150, y: 48), size: CGSize(width: 108, height: 18), badgeSide: .left))
        e.append(DiagramElement(element: "rightShoulder", kind: .shoulder, center: CGPoint(x: 410, y: 48), size: CGSize(width: 108, height: 18), badgeSide: .right))
        // Face buttons (diamond).
        let s = Self.faceSpacing, r = Self.faceRadius
        let faceSize = CGSize(width: r * 2, height: r * 2)
        e.append(DiagramElement(element: "face.north", kind: .face, center: CGPoint(x: face.x, y: face.y - s), size: faceSize, badgeSide: .above))
        e.append(DiagramElement(element: "face.south", kind: .face, center: CGPoint(x: face.x, y: face.y + s), size: faceSize, badgeSide: .below))
        e.append(DiagramElement(element: "face.west", kind: .face, center: CGPoint(x: face.x - s, y: face.y), size: faceSize, badgeSide: .left))
        e.append(DiagramElement(element: "face.east", kind: .face, center: CGPoint(x: face.x + s, y: face.y), size: faceSize, badgeSide: .right))
        // D-pad arms.
        let a = Self.dpadArm, armSize = CGSize(width: a, height: a)
        e.append(DiagramElement(element: "dpad.up", kind: .dpad, center: CGPoint(x: dpad.x, y: dpad.y - a), size: armSize, badgeSide: .above, group: "dpad"))
        e.append(DiagramElement(element: "dpad.down", kind: .dpad, center: CGPoint(x: dpad.x, y: dpad.y + a), size: armSize, badgeSide: .below, group: "dpad"))
        e.append(DiagramElement(element: "dpad.left", kind: .dpad, center: CGPoint(x: dpad.x - a, y: dpad.y), size: armSize, badgeSide: .left, group: "dpad"))
        e.append(DiagramElement(element: "dpad.right", kind: .dpad, center: CGPoint(x: dpad.x + a, y: dpad.y), size: armSize, badgeSide: .right, group: "dpad"))
        // Sticks: four direction arrows + click (the cap).
        for (name, c, click) in [("lstick", lStick, "leftThumb"), ("rstick", rStick, "rightThumb")] {
            let d = Self.stickRadius - 9, ds = CGSize(width: 15, height: 15)
            e.append(DiagramElement(element: name + ".up", kind: .stickDirection, center: CGPoint(x: c.x, y: c.y - d), size: ds, badgeSide: .above, group: name))
            e.append(DiagramElement(element: name + ".down", kind: .stickDirection, center: CGPoint(x: c.x, y: c.y + d), size: ds, badgeSide: .below, group: name))
            e.append(DiagramElement(element: name + ".left", kind: .stickDirection, center: CGPoint(x: c.x - d, y: c.y), size: ds, badgeSide: .left, group: name))
            e.append(DiagramElement(element: name + ".right", kind: .stickDirection, center: CGPoint(x: c.x + d, y: c.y), size: ds, badgeSide: .right, group: name))
            // The click badge sits on the outer side of the pad (left stick: left, right stick: right).
            e.append(DiagramElement(element: click, kind: .stickClick, center: c, size: CGSize(width: 22, height: 22),
                                    badgeSide: name == "lstick" ? .left : .right))
        }
        // Small center buttons.
        if sym {
            e.append(DiagramElement(element: "options", kind: .small, center: CGPoint(x: 227, y: 90), size: CGSize(width: 34, height: 14), badgeSide: .above))
            e.append(DiagramElement(element: "menu", kind: .small, center: CGPoint(x: 333, y: 90), size: CGSize(width: 34, height: 14), badgeSide: .above))
            e.append(DiagramElement(element: "home", kind: .home, center: CGPoint(x: 280, y: 152), size: CGSize(width: 22, height: 22), badgeSide: .below))
        } else {
            e.append(DiagramElement(element: "options", kind: .small, center: CGPoint(x: 245, y: 104), size: CGSize(width: 30, height: 14), badgeSide: .above))
            e.append(DiagramElement(element: "menu", kind: .small, center: CGPoint(x: 315, y: 104), size: CGSize(width: 30, height: 14), badgeSide: .above))
            e.append(DiagramElement(element: "home", kind: .home, center: CGPoint(x: 280, y: 134), size: CGSize(width: 22, height: 22), badgeSide: .below))
        }
        elements = e

        // Grouped badges ("Move") go below the group.
        groupAnchors = [
            "dpad": CGPoint(x: dpad.x, y: dpad.y + a * 1.5 + 4),
            "lstick": CGPoint(x: lStick.x, y: lStick.y + Self.stickRadius + 4),
            "rstick": CGPoint(x: rStick.x, y: rStick.y + Self.stickRadius + 4),
        ]
    }

    /// Every element id that can be bound on this layout (without the "gcN:" prefix).
    var bindableElements: [String] { elements.map(\.element) }

    func element(_ name: String) -> DiagramElement? { elements.first { $0.element == name } }
}

// MARK: - assignment summaries (what each element does)

enum ControllerAssignments {
    /// Actions bound to `gc<slot>:<element>`, in catalog order.
    static func actions(element: String, slot: Int, config: InputCatalog.Config) -> [String] {
        let id = "gc\(slot):\(element)"
        let bound = Set(config.bindings.filter { $0.input == id }.map(\.action))
        return InputCatalog.allActions.map(\.id).filter(bound.contains)
    }

    /// Human name of an element on a family: "Right Button (A)", "D-pad ↑", "ZL"...
    static func title(element: String, family: ControllerFamily, labels: [String: String] = [:]) -> String {
        let label = labels[element] ?? family.label(element)
        let arrows = ["up": "↑", "down": "↓", "left": "←", "right": "→"]
        let parts = element.split(separator: ".").map(String.init)
        if parts.count == 2, let arrow = arrows[parts[1]] {
            let base = ["dpad": String(localized: "D-pad"), "lstick": String(localized: "Left Stick"), "rstick": String(localized: "Right Stick")][parts[0]] ?? parts[0]
            return base + " " + arrow
        }
        let positional = ["face.south": String(localized: "Bottom Button"), "face.east": String(localized: "Right Button"),
                          "face.west": String(localized: "Left Button"), "face.north": String(localized: "Top Button"),
                          "leftThumb": String(localized: "Left Stick Press"), "rightThumb": String(localized: "Right Stick Press")]
        if let p = positional[element] { return label.isEmpty ? p : String(localized: "\(p) (\(label))") }
        return label.isEmpty ? element : label
    }

    /// Compact label for a badge: "A", "Turbo A", "Rewind", "2P B"...
    static func shortLabel(_ action: String, slot: Int) -> String {
        let hk: [String: String] = [
            "hk.rewind": String(localized: "Rewind"), "hk.fast_forward": String(localized: "Fast Fwd"),
            "hk.pause": String(localized: "Pause"), "hk.slow": String(localized: "Slow"),
            "hk.frame_advance": String(localized: "Advance"), "hk.step_back": String(localized: "Step Back"),
            "hk.toggle_mode": String(localized: "Rec/Play"), "hk.bookmark": String(localized: "Bookmark"),
            "hk.undo_take": String(localized: "Prev Take"), "hk.save": String(localized: "Save"),
            "hk.soft_reset": String(localized: "Reset"), "hk.power_cycle": String(localized: "Power"),
        ]
        if let h = hk[action] { return h }
        let parts = action.split(separator: ".")
        guard parts.count == 2 else { return action }
        let game: [String: String] = [
            "a": "A", "b": "B", "select": "SELECT", "start": "START", "up": "↑", "down": "↓", "left": "←", "right": "→",
            "turbo_a": String(localized: "Turbo A"), "turbo_b": String(localized: "Turbo B"),
        ]
        let name = game[String(parts[1])] ?? String(parts[1])
        let player = parts[0] == "p2" ? 1 : 0
        return player == slot ? name : "\(player + 1)P \(name)"
    }

    static func badge(element: String, slot: Int, config: InputCatalog.Config) -> String? {
        let a = actions(element: element, slot: slot, config: config)
        return a.isEmpty ? nil : a.map { shortLabel($0, slot: slot) }.joined(separator: String(localized: " · "))
    }

    /// For "dpad" / "lstick" / "rstick": "Move" (or "2P Move") when the four directions map 1:1
    /// to one player's directions, nil when nothing is bound. `.custom` otherwise.
    enum GroupSummary: Equatable { case none, movement(String), custom }

    static func group(_ group: String, slot: Int, config: InputCatalog.Config) -> GroupSummary {
        let dirs = ["up", "down", "left", "right"]
        let per = dirs.map { actions(element: "\(group).\($0)", slot: slot, config: config) }
        if per.allSatisfy(\.isEmpty) { return .none }
        for p in ["p1", "p2"] where zip(dirs, per).allSatisfy({ $1 == ["\(p).\($0)"] }) {
            let player = p == "p2" ? 1 : 0
            return .movement(player == slot ? String(localized: "Move") : String(localized: "\(player + 1)P Move"))
        }
        return .custom
    }
}
