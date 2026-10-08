// Structure of the Quick Menu (docs/design/UI_REDESIGN.md): the six top-level tiles, their
// sub-pages, how each page is laid out and how focus moves. Pure (no app state), so the rules
// "≤ 6 items per page", "max depth 3" and "no page scrolls" are unit-tested
// (Tests/QuickMenuModelTests.swift); the items themselves are built by the app
// (Sources/App/QuickMenu/QuickMenuPages.swift).
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum QMPage: String, CaseIterable, Hashable {
    case top
    case retry, takes, bookmarks
    case practice
    case share, stream
    case display, crtDetail
    case controls, controller, bindings, controlsDetail
    case sound
    case system, updates, about
    case game, reset

    /// The four settings pages (L / R switch between them; the Settings tile opens the first).
    static let settingsTabs: [QMPage] = [.display, .controls, .sound, .system]

    var parent: QMPage? {
        switch self {
        case .top: return nil
        case .retry, .practice, .share, .game: return .top
        case .display, .controls, .sound, .system: return .top
        case .takes, .bookmarks: return .retry
        case .stream: return .share
        case .crtDetail: return .display
        case .controller, .bindings, .controlsDetail: return .controls
        case .updates, .about: return .system
        case .reset: return .game
        }
    }

    /// Pages from the top level to this one (top first).
    var path: [QMPage] {
        var out: [QMPage] = [self]
        var p = parent
        while let x = p { out.insert(x, at: 0); p = x.parent }
        return out
    }

    /// Levels below the top (Settings › Display › CRT Details = 2: the settings tabs are one level).
    var depth: Int { path.count - 1 }

    var isSettings: Bool { path.contains { QMPage.settingsTabs.contains($0) } }

    enum Layout: Equatable {
        case tiles(columns: Int)   // top level: one row of large tiles
        case actions               // sub-level: up to 6 medium tiles (3 x 2)
        case rows                  // settings: up to 6 rows of label | value
        case cards(columns: Int, rows: Int)   // thumbnails (practice slots, takes, bookmarks)
        case custom                // controller diagram
    }

    var layout: Layout {
        switch self {
        case .top: return .tiles(columns: 6)
        case .retry, .share, .game, .reset: return .actions
        case .practice: return .cards(columns: 4, rows: 2)
        case .takes, .bookmarks: return .cards(columns: 3, rows: 2)
        case .controller: return .custom
        default: return .rows
        }
    }

    /// Items shown at once (longer lists are split into pages switched with L / R).
    var capacity: Int {
        switch layout {
        case .tiles(let c): return c
        case .actions, .rows: return 6
        case .cards(let c, let r): return c * r
        case .custom: return 1
        }
    }

    var columns: Int {
        switch layout {
        case .tiles(let c): return c
        case .actions: return 3
        case .rows, .custom: return 1
        case .cards(let c, _): return c
        }
    }

    /// Long lists (takes, bookmarks, bindings) are paged; L / R switch pages.
    var paged: Bool { self == .takes || self == .bookmarks || self == .bindings || self == .controller }

    var icon: String {
        switch self {
        case .top: return "line.3.horizontal"
        case .retry, .takes, .bookmarks: return "clock.arrow.circlepath"
        case .practice: return "target"
        case .share, .stream: return "square.and.arrow.up"
        case .display, .crtDetail, .controls, .controller, .bindings, .controlsDetail, .sound, .system, .updates, .about:
            return "gearshape"
        case .game, .reset: return "gamecontroller"
        }
    }

    var title: String {
        switch self {
        case .top: return String(localized: "Menu")
        case .retry: return String(localized: "Retry")
        case .takes: return String(localized: "Takes")
        case .bookmarks: return String(localized: "Bookmarks")
        case .practice: return String(localized: "Practice")
        case .share: return String(localized: "Share")
        case .stream: return String(localized: "Stream Output")
        case .display: return String(localized: "Display")
        case .crtDetail: return String(localized: "CRT Details")
        case .controls: return String(localized: "Controls")
        case .controller: return String(localized: "Controller")
        case .bindings: return String(localized: "Keyboard")
        case .controlsDetail: return String(localized: "Controls Details")
        case .sound: return String(localized: "Sound")
        case .system: return String(localized: "System")
        case .updates: return String(localized: "Updates")
        case .about: return String(localized: "About")
        case .game: return String(localized: "Game")
        case .reset: return String(localized: "Reset")
        }
    }
}

enum NavDirection { case up, down, left, right }

enum QuickMenuNav {
    /// Focus after a move in a grid of `count` items laid out row by row in `columns` columns.
    /// Returns nil when the move leaves the grid (e.g. left on the first column).
    static func move(_ index: Int, count: Int, columns: Int, _ d: NavDirection) -> Int? {
        guard count > 0 else { return nil }
        let i = min(max(0, index), count - 1)
        let c = max(1, columns)
        switch d {
        case .left: return i % c == 0 ? nil : i - 1
        case .right: return (i % c == c - 1 || i + 1 >= count) ? nil : i + 1
        case .up: return i - c >= 0 ? i - c : nil
        case .down:
            if i + c < count { return i + c }
            // Into a shorter last row: its last item.
            let lastRowStart = (count - 1) / c * c
            return i < lastRowStart ? count - 1 : nil
        }
    }

    static func pageCount(items: Int, perPage: Int) -> Int { max(1, (items + max(1, perPage) - 1) / max(1, perPage)) }

    /// The items of page `page` (clamped).
    static func pageRange(items: Int, perPage: Int, page: Int) -> Range<Int> {
        let pages = pageCount(items: items, perPage: perPage)
        let p = min(max(0, page), pages - 1)
        let start = p * perPage
        return start..<min(items, start + perPage)
    }
}
