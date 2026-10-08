// Controller families, positional face-button ids and the geometry of the settings diagram.
//
// Face buttons are bound by POSITION ("gc<slot>:face.south/east/west/north"), never by label:
// GameController.framework names Xbox / PlayStation / MFi face buttons by position but Nintendo
// controllers by their printed label. InputManager converts with `GCFaceMapping` so NES A is
// always the east button and NES B the south button. The rules and the diagram geometry live in
// the shared frontend core (frontend/src/input.cpp, diagram.cpp); this file adapts them.
// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import Foundation

enum FacePosition: String, CaseIterable {
    case south, east, west, north
    /// Physical element name (the part of the id after "gc<slot>:").
    var element: String { "face." + rawValue }

    var cValue: rnf_face_position {
        switch self {
        case .south: return RNF_FACE_SOUTH
        case .east: return RNF_FACE_EAST
        case .west: return RNF_FACE_WEST
        case .north: return RNF_FACE_NORTH
        }
    }

    init?(_ c: Int32) {
        switch rnf_face_position(rawValue: UInt32(bitPattern: c)) {
        case RNF_FACE_SOUTH: self = .south
        case RNF_FACE_EAST: self = .east
        case RNF_FACE_WEST: self = .west
        case RNF_FACE_NORTH: self = .north
        default: return nil
        }
    }
}

enum ControllerFamily: String, CaseIterable, Identifiable {
    case nintendo, xbox, playStation, generic, steamDeck
    var id: String { rawValue }

    var cValue: rnf_controller_family {
        switch self {
        case .nintendo: return RNF_FAMILY_NINTENDO
        case .xbox: return RNF_FAMILY_XBOX
        case .playStation: return RNF_FAMILY_PLAYSTATION
        case .generic: return RNF_FAMILY_GENERIC
        case .steamDeck: return RNF_FAMILY_STEAM_DECK
        }
    }

    init(_ c: rnf_controller_family) {
        switch c {
        case RNF_FAMILY_NINTENDO: self = .nintendo
        case RNF_FAMILY_XBOX: self = .xbox
        case RNF_FAMILY_PLAYSTATION: self = .playStation
        case RNF_FAMILY_STEAM_DECK: self = .steamDeck
        default: self = .generic
        }
    }

    var title: String { rnfString(rnf_controller_family_title(cValue)) }

    /// From GCDevice.productCategory (e.g. "Switch Pro Controller", "Xbox One", "DualSense") and
    /// vendorName as a fallback.
    static func from(productCategory: String, vendorName: String? = nil) -> ControllerFamily {
        ControllerFamily(rnf_controller_family_from(productCategory, vendorName))
    }

    /// The printed label of a physical element on this family (fallback when the device does not
    /// report one).
    func label(_ element: String) -> String { String(cString: rnf_controller_family_label(cValue, element)) }

    /// Diagram arrangement: PlayStation has both sticks at the bottom.
    var isSymmetric: Bool { rnf_controller_family_is_symmetric(cValue) != 0 }
}

/// Where GameController.framework's buttonA/B/X/Y physically are.
enum GCFaceMapping {
    static let buttons = ["buttonA", "buttonB", "buttonX", "buttonY"]

    /// `symbols`: GCControllerElement.sfSymbolsName per GameController button ("buttonA" ...).
    /// Nintendo-layout pads are placed by their printed glyph when the device reports all four.
    static func positions(productCategory: String, vendorName: String? = nil,
                          symbols: [String: String?] = [:]) -> [String: FacePosition] {
        var out: [Int32] = [0, 0, 0, 0]
        withCStrings(buttons.map { (symbols[$0] ?? nil) ?? "" }) { c in
            let syms: [UnsafePointer<CChar>?] = buttons.indices.map { ((symbols[buttons[$0]] ?? nil) == nil) ? nil : c[$0] }
            rnf_gc_face_positions(productCategory, vendorName, syms, &out)
        }
        var m: [String: FacePosition] = [:]
        for (i, b) in buttons.enumerated() { m[b] = FacePosition(out[i]) }
        return m
    }

    /// Micro gamepads (Siri Remote and the like): A = primary, X = secondary.
    static let micro: [String: FacePosition] = ["buttonA": .south, "buttonX": .west]

    /// Text for a face-button glyph reported by the device (GCControllerElement.sfSymbolsName).
    static func label(fromSymbol name: String?) -> String? {
        guard let name, let l = rnf_gc_label_from_symbol(name) else { return nil }
        return String(cString: l)
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
    /// Point where the assignment badge is anchored (badge grows away from the element).
    let badgeAnchor: CGPoint
    var id: String { element }

    var frame: CGRect { CGRect(x: center.x - size.width / 2, y: center.y - size.height / 2, width: size.width, height: size.height) }

    init(_ e: rnf_diagram_element) {
        element = String(cString: e.element)
        switch e.kind {
        case RNF_DIAGRAM_FACE: kind = .face
        case RNF_DIAGRAM_DPAD: kind = .dpad
        case RNF_DIAGRAM_STICK_DIRECTION: kind = .stickDirection
        case RNF_DIAGRAM_STICK_CLICK: kind = .stickClick
        case RNF_DIAGRAM_SHOULDER: kind = .shoulder
        case RNF_DIAGRAM_TRIGGER: kind = .trigger
        case RNF_DIAGRAM_SMALL: kind = .small
        default: kind = .home
        }
        center = CGPoint(x: e.cx, y: e.cy)
        size = CGSize(width: e.width, height: e.height)
        switch e.badge_side {
        case RNF_SIDE_LEFT: badgeSide = .left
        case RNF_SIDE_RIGHT: badgeSide = .right
        case RNF_SIDE_ABOVE: badgeSide = .above
        default: badgeSide = .below
        }
        group = e.group.map { String(cString: $0) }
        badgeAnchor = CGPoint(x: e.badge_x, y: e.badge_y)
    }
}

struct ControllerDiagramLayout {
    static let canvas = CGSize(width: RNF_DIAGRAM_CANVAS_WIDTH, height: RNF_DIAGRAM_CANVAS_HEIGHT)

    let family: ControllerFamily
    let elements: [DiagramElement]
    let leftStick: (center: CGPoint, radius: CGFloat)
    let rightStick: (center: CGPoint, radius: CGFloat)
    let dpadCenter: CGPoint
    let faceCenter: CGPoint
    /// The outline under the elements, back to front (rnf_diagram_decor): the body (a rounded pill;
    /// the Steam Deck: a wide rounded rectangle), its screen (Steam Deck) and touch / track pads.
    struct Decor: Equatable {
        enum Kind { case body, screen, pad }
        let kind: Kind
        let rect: CGRect
        let radius: CGFloat
    }
    let decor: [Decor]
    /// Grouped badge anchors (below the D-pad / sticks) used when a group has a standard mapping.
    let groupAnchors: [String: CGPoint]

    static let stickRadius = CGFloat(RNF_DIAGRAM_STICK_RADIUS)
    static let faceSpacing = CGFloat(RNF_DIAGRAM_FACE_SPACING)
    static let faceRadius = CGFloat(RNF_DIAGRAM_FACE_RADIUS)
    static let dpadArm = CGFloat(RNF_DIAGRAM_DPAD_ARM)

    init(family: ControllerFamily) {
        self.family = family
        let f = family.cValue
        var i = rnf_diagram_info()
        rnf_diagram_info_get(f, &i)
        leftStick = (CGPoint(x: i.left_stick_x, y: i.left_stick_y), CGFloat(i.stick_radius))
        rightStick = (CGPoint(x: i.right_stick_x, y: i.right_stick_y), CGFloat(i.stick_radius))
        dpadCenter = CGPoint(x: i.dpad_x, y: i.dpad_y)
        faceCenter = CGPoint(x: i.face_x, y: i.face_y)
        decor = (0..<rnf_diagram_decor_count(f)).compactMap { n in
            var d = rnf_diagram_decor()
            guard rnf_diagram_decor_get(f, n, &d) != 0 else { return nil }
            let kind: Decor.Kind = d.kind == RNF_DECOR_BODY ? .body : d.kind == RNF_DECOR_SCREEN ? .screen : .pad
            return Decor(kind: kind, rect: CGRect(x: d.x, y: d.y, width: d.width, height: d.height), radius: CGFloat(d.radius))
        }
        elements = (0..<rnf_diagram_element_count(f)).compactMap { n in
            var e = rnf_diagram_element()
            return rnf_diagram_element_get(f, n, &e) != 0 ? DiagramElement(e) : nil
        }
        groupAnchors = [
            "dpad": CGPoint(x: i.dpad_anchor_x, y: i.dpad_anchor_y),
            "lstick": CGPoint(x: i.lstick_anchor_x, y: i.lstick_anchor_y),
            "rstick": CGPoint(x: i.rstick_anchor_x, y: i.rstick_anchor_y),
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
        withBindings(config.bindings) { b, n in rnfPairs(rnf_input_element_actions(b, n, element, Int32(slot))).map(\.0) }
    }

    /// Human name of an element on a family: "Right Button (A)", "D-pad ↑", "ZL"...
    static func title(element: String, family: ControllerFamily, labels: [String: String] = [:]) -> String {
        rnfString(rnf_input_element_title(element, family.cValue, labels[element]))
    }

    /// Compact label for a badge: "A", "Turbo A", "Rewind", "2P B"...
    static func shortLabel(_ action: String, slot: Int) -> String {
        rnfString(rnf_input_action_short_label(action, Int32(slot)))
    }

    static func badge(element: String, slot: Int, config: InputCatalog.Config) -> String? {
        withBindings(config.bindings) { b, n in rnfTake(rnf_input_element_badge(b, n, element, Int32(slot))) }
    }

    /// For "dpad" / "lstick" / "rstick": "Move" (or "2P Move") when the four directions map 1:1
    /// to one player's directions, nil when nothing is bound. `.custom` otherwise.
    enum GroupSummary: Equatable { case none, movement(String), custom }

    static func group(_ group: String, slot: Int, config: InputCatalog.Config) -> GroupSummary {
        withBindings(config.bindings) { b, n in
            var text: UnsafeMutablePointer<CChar>?
            switch rnf_input_group_summary(b, n, group, Int32(slot), &text) {
            case RNF_GROUP_MOVEMENT: return .movement(rnfString(text))
            case RNF_GROUP_CUSTOM: return .custom
            default: return GroupSummary.none
            }
        }
    }
}
