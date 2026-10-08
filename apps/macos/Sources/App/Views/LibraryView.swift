// Library = the start screen (docs/design/UI_REDESIGN.md, "Library"): a large "Continue" card,
// then big game cards (4 per row, paged with L / R instead of scrolling), or one card saying where
// to put ROMs. ROMs live in ~/Documents/ReplayNES/ROM, their projects (matched by SHA-256) in
// Projects. Controller / keyboard: D-pad moves, A plays, X = projects of the game, Y = search.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Focus and paging of the library (main thread).
final class LibraryNav: ObservableObject {
    /// -1 = the Continue card.
    @Published var focus = 0
    @Published var page = 0
    @Published var search = ""
    @Published var searchRequested = 0
    /// X: the projects list of this ROM.
    @Published var projectsOf: LibraryROM?
    @Published var projectsFocus = 0
    /// Set by the view from its size.
    var columns = 4
    var perPage = 8

    func entries(_ library: LibraryModel) -> [LibraryROM] {
        let q = search.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { return library.roms }
        return library.roms.filter { $0.name.localizedStandardContains(q) || $0.relativePath.localizedStandardContains(q) }
    }

    func pageCount(_ n: Int) -> Int { QuickMenuNav.pageCount(items: n, perPage: perPage) }

    func handle(_ n: NavInput, model m: AppModel) {
        let library = m.library
        if let rom = projectsOf {
            let rows = LibraryHome.projectRows(rom, model: m)
            switch n {
            case .up: projectsFocus = max(0, projectsFocus - 1)
            case .down: projectsFocus = min(rows.count - 1, projectsFocus + 1)
            case .confirm: if rows.indices.contains(projectsFocus) { projectsOf = nil; rows[projectsFocus].run() }
            case .back, .escape, .x: projectsOf = nil
            default: break
            }
            return
        }
        let list = entries(library)
        let hero = LibraryHome.continueItem(m) != nil
        if list.isEmpty {
            // The Continue card (if any) and the "where to put ROMs" card.
            switch n {
            case .up: if hero { focus = -1 }
            case .down: focus = 0
            case .confirm: if focus < 0 && hero { LibraryHome.continueItem(m)?.run() } else { library.revealROMFolder() }
            case .y: searchRequested += 1
            case .back, .escape: if m.status.hasSession { m.hideLibraryScreen() }
            default: break
            }
            return
        }
        let range = QuickMenuNav.pageRange(items: list.count, perPage: perPage, page: page)
        let local = focus - range.lowerBound
        switch n {
        case .up:
            if focus < 0 { return }
            if local < columns { if hero { focus = -1 } } else { focus -= columns }
        case .down:
            if focus < 0 { if !range.isEmpty { focus = range.lowerBound } ; return }
            if let j = QuickMenuNav.move(local, count: range.count, columns: columns, .down) { focus = range.lowerBound + j }
        case .left, .right:
            guard focus >= 0 else { return }
            let d: NavDirection = n == .left ? .left : .right
            if let j = QuickMenuNav.move(local, count: range.count, columns: columns, d) { focus = range.lowerBound + j }
            else { flip(n == .left ? -1 : 1, count: list.count) }
        case .pagePrev, .pageNext:
            flip(n == .pagePrev ? -1 : 1, count: list.count)
        case .confirm:
            if focus < 0 { LibraryHome.continueItem(m)?.run() }
            else if list.indices.contains(focus) { LibraryHome.play(list[focus], model: m) }
        case .x:
            if focus >= 0, list.indices.contains(focus) { projectsFocus = 0; projectsOf = list[focus] }
        case .y:
            searchRequested += 1
        case .back, .escape:
            if m.status.hasSession { m.hideLibraryScreen() }
        case .menu:
            break
        }
    }

    private func flip(_ by: Int, count: Int) {
        let pages = pageCount(count)
        guard pages > 1 else { return }
        withAnimation(QMStyle.anim) {
            page = (page + by + pages) % pages
            focus = QuickMenuNav.pageRange(items: count, perPage: perPage, page: page).lowerBound
        }
    }

    /// Keeps focus and page valid when the list changes.
    func clamp(count: Int, hero: Bool) {
        let pages = pageCount(count)
        if page >= pages { page = pages - 1 }
        if count == 0 { focus = hero ? -1 : 0; return }
        if focus < 0 && !hero { focus = 0 }
        if focus >= count { focus = count - 1 }
        if focus >= 0 {
            let r = QuickMenuNav.pageRange(items: count, perPage: perPage, page: page)
            if !r.contains(focus) { page = focus / max(1, perPage) }
        }
    }
}

struct LibraryHome: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var library: LibraryModel
    @ObservedObject var nav: LibraryNav
    @ObservedObject private var monitor = AppModel.shared.input.controllerMonitor
    @FocusState private var searchFocused: Bool

    static let gap: CGFloat = 14
    static let heroHeight: CGFloat = 150

    struct ContinueItem {
        let title: String
        let subtitle: String
        let image: CGImage?
        let run: () -> Void
    }

    var body: some View {
        GeometryReader { geo in
            let list = nav.entries(library)
            let hero = Self.continueItem(model)
            let inner = geo.size.width - 2 * QMStyle.margin
            let cardW = (inner - 3 * Self.gap) / 4
            let cardH = cardW * 0.56 + 52
            let fixed: CGFloat = 2 * QMStyle.margin + 34 + 18 + (hero != nil ? Self.heroHeight + 18 : 0) + 28 + 18
            let rows = max(1, Int((geo.size.height - fixed + Self.gap) / (cardH + Self.gap)))
            let per = rows * 4
            VStack(alignment: .leading, spacing: 18) {
                header
                    .padding(.trailing, 150)   // the menu pill
                if let err = library.folderError { banner(err, retry: true) } else if let err = library.scanError { banner(err, retry: false) }
                if list.isEmpty && hero == nil && nav.search.isEmpty {
                    emptyCard.frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    if let hero { heroCard(hero) }
                    if list.isEmpty && nav.search.isEmpty {
                        emptyCard.frame(maxWidth: .infinity)
                    } else {
                        grid(list, cardW: cardW, cardH: cardH, perPage: per)
                    }
                    Spacer(minLength: 0)
                    footer(list.count)
                }
            }
            .padding(QMStyle.margin)
            .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
            .onAppear { nav.perPage = per; nav.clamp(count: list.count, hero: hero != nil); library.refresh() }
            .onChange(of: per) { _, p in nav.perPage = p; nav.clamp(count: list.count, hero: hero != nil) }
            .onChange(of: list.count) { _, c in nav.clamp(count: c, hero: hero != nil) }
        }
        .background(Color(red: 0.055, green: 0.055, blue: 0.063))
        .overlay { if let rom = nav.projectsOf { projectsPanel(rom) } }
        .environment(\.colorScheme, .dark)
        .foregroundStyle(.white)
        .onChange(of: nav.searchRequested) { _, _ in searchFocused = true }
    }

    // MARK: header / footer

    private var header: some View {
        HStack(spacing: 12) {
            Text("ReplayNES").font(.system(size: 22, weight: .bold))
            if library.scanning { ProgressView().controlSize(.small) }
            Spacer()
            if !library.roms.isEmpty { searchField }
        }
        .frame(height: 34)
    }

    private var searchField: some View {
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass").foregroundStyle(.white.opacity(0.5))
                TextField("Search", text: $nav.search)
                    .textFieldStyle(.plain).frame(width: 150)
                    .focused($searchFocused)
                    .onSubmit { searchFocused = false; nav.focus = 0; nav.page = 0 }
                if !nav.search.isEmpty {
                    Image(systemName: "xmark.circle.fill").foregroundStyle(.white.opacity(0.5))
                        .onTapGesture { nav.search = "" }
                }
            }
            .font(.system(size: 13))
            .padding(.horizontal, 10).frame(height: 30)
            .background(Capsule().fill(Color.white.opacity(searchFocused ? 0.14 : 0.08)))
    }

    private func footer(_ count: Int) -> some View {
        let g = HintGlyphs.current(monitor.controllers)
        let pages = nav.pageCount(count)
        return HStack(spacing: 14) {
            if pages > 1 {
                KeyCap(g.l).onTapGesture { nav.handle(.pagePrev, model: model) }
                HStack(spacing: 5) {
                    ForEach(0..<pages, id: \.self) { i in
                        Circle().fill(i == nav.page ? Color.white : Color.white.opacity(0.3)).frame(width: 6, height: 6)
                    }
                }
                KeyCap(g.r).onTapGesture { nav.handle(.pageNext, model: model) }
            }
            Spacer()
            if count > 0 {
                hint(g.confirm, String(localized: "Play"))
                hint(g.x, String(localized: "Projects"))
                hint(g.y, String(localized: "Search"))
            }
        }
        .frame(height: 28)
    }

    private func hint(_ glyph: String, _ text: String) -> some View {
        HStack(spacing: 5) { KeyCap(glyph); Text(text).font(QMStyle.hint).foregroundStyle(.white.opacity(0.8)) }
    }

    private func banner(_ text: String, retry: Bool) -> some View {
        HStack(alignment: .top) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            Text(text).font(.callout).lineLimit(3).textSelection(.enabled)
            Spacer()
            if retry { Button("Retry") { library.start() } }
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color.orange.opacity(0.15)))
    }

    // MARK: Continue

    static func continueItem(_ m: AppModel) -> ContinueItem? {
        if m.status.hasSession {
            let rom = URL(fileURLWithPath: m.status.romPath).deletingPathExtension().lastPathComponent
            return ContinueItem(title: rom, subtitle: String(localized: "Playing now"),
                                image: ThumbnailStore.image(project: m.status.projectPath) ?? ThumbnailStore.image(romSHA: m.current?.romSHA256),
                                run: { m.hideLibraryScreen() })
        }
        let all = m.library.projectsBySHA.values.flatMap { $0 }
        guard let p = all.max(by: { $0.modified < $1.modified }) else { return nil }
        let name = URL(fileURLWithPath: p.romName).deletingPathExtension().lastPathComponent
        return ContinueItem(title: name.isEmpty ? p.name : name,
                            subtitle: p.modified.formatted(.relative(presentation: .named)),
                            image: ThumbnailStore.image(project: p.url.path) ?? ThumbnailStore.image(romSHA: p.romSHA256),
                            run: { m.continueProject(p.url) })
    }

    private func heroCard(_ c: ContinueItem) -> some View {
        let focused = nav.focus < 0
        let g = HintGlyphs.current(monitor.controllers)
        return HStack(spacing: 18) {
            GameArt(image: c.image, name: c.title)
                .frame(width: (Self.heroHeight - 24) * 256 / 224, height: Self.heroHeight - 24)
                .clipShape(RoundedRectangle(cornerRadius: 8))
            VStack(alignment: .leading, spacing: 6) {
                Text("Continue").font(.system(size: 12, weight: .bold)).foregroundStyle(QMStyle.brand).textCase(.uppercase)
                Text(c.title).font(.system(size: 22, weight: .bold)).lineLimit(1)
                Text(c.subtitle).font(.system(size: 13)).foregroundStyle(.white.opacity(0.6)).lineLimit(1)
                Spacer(minLength: 0)
                hint(g.confirm, String(localized: "Continue"))
            }
            .padding(.vertical, 4)
            Spacer()
        }
        .padding(12)
        .frame(height: Self.heroHeight)
        .background(RoundedRectangle(cornerRadius: QMStyle.radius).fill(focused ? QMStyle.tileFocused : QMStyle.tile))
        .overlay(RoundedRectangle(cornerRadius: QMStyle.radius).strokeBorder(focused ? QMStyle.brand : .clear, lineWidth: 2))
        .contentShape(Rectangle())
        .onHover { if $0 { nav.focus = -1 } }
        .onTapGesture { nav.focus = -1; c.run() }
        .animation(QMStyle.anim, value: focused)
    }

    // MARK: game cards

    private func grid(_ list: [LibraryROM], cardW: CGFloat, cardH: CGFloat, perPage: Int) -> some View {
        let range = QuickMenuNav.pageRange(items: list.count, perPage: perPage, page: nav.page)
        let idx = Array(range)
        let rows = stride(from: 0, to: idx.count, by: 4).map { Array(idx[$0..<min(idx.count, $0 + 4)]) }
        return VStack(alignment: .leading, spacing: Self.gap) {
            if list.isEmpty {
                Text("No games match “\(nav.search)”").foregroundStyle(.white.opacity(0.6)).frame(height: cardH)
            }
            ForEach(rows, id: \.first) { row in
                HStack(spacing: Self.gap) {
                    ForEach(row, id: \.self) { i in
                        gameCard(list[i], index: i, width: cardW, artHeight: cardH - 52).frame(width: cardW, height: cardH)
                    }
                }
            }
        }
        .id(nav.page)
        .transition(.opacity)
    }

    private func gameCard(_ rom: LibraryROM, index i: Int, width: CGFloat, artHeight: CGFloat) -> some View {
        let focused = nav.focus == i
        let projects = library.projects(for: rom)
        let sub = projects.isEmpty ? String(localized: "New")
            : String(localized: "\(projects.count) projects · \(projects[0].modified.formatted(.relative(presentation: .named)))")
        return VStack(alignment: .leading, spacing: 0) {
            GameArt(image: ThumbnailStore.image(romSHA: rom.sha256), name: rom.name)
                .frame(width: width, height: artHeight)
                .clipped()
            VStack(alignment: .leading, spacing: 2) {
                Text(rom.name).font(.system(size: 15, weight: .semibold)).lineLimit(1)
                Text(sub).font(QMStyle.hint).foregroundStyle(.white.opacity(0.55)).lineLimit(1)
            }
            .padding(.horizontal, 10).padding(.vertical, 8)
            .frame(height: 52, alignment: .leading)
        }
        .clipShape(RoundedRectangle(cornerRadius: QMStyle.tileRadius))
        .background(RoundedRectangle(cornerRadius: QMStyle.tileRadius).fill(focused ? QMStyle.tileFocused : QMStyle.tile))
        .overlay(RoundedRectangle(cornerRadius: QMStyle.tileRadius).strokeBorder(focused ? QMStyle.brand : .clear, lineWidth: 2))
        .scaleEffect(focused ? 1.03 : 1)
        .animation(QMStyle.anim, value: focused)
        .contentShape(Rectangle())
        .onHover { if $0 { nav.focus = i } }
        .onTapGesture { nav.focus = i; Self.play(rom, model: model) }
        .contextMenu {
            Button("Projects…") { nav.projectsFocus = 0; nav.projectsOf = rom }
            Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([rom.url]) }
        }
    }

    /// A on a game: continue its newest project, or start one.
    static func play(_ rom: LibraryROM, model m: AppModel) {
        if let p = m.library.projects(for: rom).first {
            if m.continueProject(p.url) { m.hideLibraryScreen() }
        } else if m.playFromLibrary(rom) {
            m.hideLibraryScreen()
        }
    }

    // MARK: projects of a game (X)

    struct ProjectRow { let title: String; let subtitle: String; let icon: String; let run: () -> Void }

    static func projectRows(_ rom: LibraryROM, model m: AppModel) -> [ProjectRow] {
        var rows = [ProjectRow(title: String(localized: "New Game"), subtitle: String(localized: "Start a new project from power-on"),
                               icon: "plus", run: { if m.playFromLibrary(rom) { m.hideLibraryScreen() } })]
        for p in m.library.projects(for: rom).prefix(5) {
            rows.append(ProjectRow(title: p.name, subtitle: p.modified.formatted(date: .abbreviated, time: .shortened), icon: "film.stack",
                                   run: { if m.continueProject(p.url) { m.hideLibraryScreen() } }))
        }
        return rows
    }

    private func projectsPanel(_ rom: LibraryROM) -> some View {
        let rows = Self.projectRows(rom, model: model)
        let g = HintGlyphs.current(monitor.controllers)
        return ZStack {
            Color.black.opacity(0.55).contentShape(Rectangle()).onTapGesture { nav.projectsOf = nil }
            VStack(alignment: .leading, spacing: 12) {
                Text(rom.name).font(QMStyle.title).lineLimit(1)
                VStack(spacing: QMStyle.rowGap) {
                    ForEach(Array(rows.enumerated()), id: \.offset) { i, r in
                        let f = i == nav.projectsFocus
                        HStack(spacing: 12) {
                            Image(systemName: r.icon).frame(width: 22).foregroundStyle(f ? QMStyle.brand : .white.opacity(0.6))
                            Text(r.title).font(QMStyle.label).lineLimit(1).truncationMode(.middle)
                            Spacer()
                            Text(r.subtitle).font(QMStyle.hint).foregroundStyle(.white.opacity(0.55)).lineLimit(1)
                        }
                        .padding(.horizontal, 14).frame(height: QMStyle.rowHeight)
                        .background(RoundedRectangle(cornerRadius: 10).fill(f ? QMStyle.tileFocused : QMStyle.tile))
                        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(f ? QMStyle.brand : .clear, lineWidth: 2))
                        .contentShape(Rectangle())
                        .onHover { if $0 { nav.projectsFocus = i } }
                        .onTapGesture { nav.projectsOf = nil; r.run() }
                    }
                }
                HStack(spacing: 14) {
                    Spacer()
                    hint(g.confirm, String(localized: "Open"))
                    hint(g.back, String(localized: "Back")).onTapGesture { nav.projectsOf = nil }
                }
            }
            .padding(QMStyle.margin)
            .frame(width: 560)
            .background(RoundedRectangle(cornerRadius: QMStyle.radius).fill(QMStyle.panel))
            .overlay(RoundedRectangle(cornerRadius: QMStyle.radius).stroke(Color.white.opacity(0.08)))
        }
    }

    // MARK: empty library

    private var emptyCard: some View {
        let g = HintGlyphs.current(monitor.controllers)
        return VStack(spacing: 14) {
            Image(systemName: "tray.and.arrow.down").font(.system(size: 40, weight: .light)).foregroundStyle(QMStyle.brand)
            Text("Add Your Games").font(QMStyle.title)
            Text("Put .nes files in this folder and they appear here.").font(.system(size: 14)).foregroundStyle(.white.opacity(0.7))
            Text(verbatim: displayPath(library.paths.roms))
                .font(.system(size: 13, design: .monospaced)).foregroundStyle(.white.opacity(0.85))
                .lineLimit(1).truncationMode(.middle).textSelection(.enabled)
                .padding(.horizontal, 12).padding(.vertical, 6)
                .background(RoundedRectangle(cornerRadius: 8).fill(Color.black.opacity(0.35)))
            HStack(spacing: 8) {
                KeyCap(g.confirm)
                Text("Open Folder").font(QMStyle.label)
            }
            .padding(.horizontal, 16).frame(height: 38)
            .background(RoundedRectangle(cornerRadius: 10).fill(QMStyle.brand.opacity(0.9)))
            .contentShape(Rectangle())
            .onTapGesture { library.revealROMFolder() }
        }
        .padding(32)
        .frame(maxWidth: 560)
        .background(RoundedRectangle(cornerRadius: QMStyle.radius).fill(nav.focus >= 0 ? QMStyle.tileFocused : QMStyle.tile))
        .overlay(RoundedRectangle(cornerRadius: QMStyle.radius).strokeBorder(nav.focus >= 0 ? QMStyle.brand : .clear, lineWidth: 2))
        .onHover { if $0 { nav.focus = 0 } }
        .animation(QMStyle.anim, value: nav.focus)
    }

    private func displayPath(_ url: URL) -> String {
        let home = NSHomeDirectory()
        return url.path.hasPrefix(home) ? "~" + url.path.dropFirst(home.count) : url.path
    }
}

/// A game's picture: the last session's thumbnail, else a tile made from its name.
struct GameArt: View {
    let image: CGImage?
    let name: String
    var body: some View {
        if let image {
            Image(decorative: image, scale: 1).interpolation(.none).resizable().scaledToFill()
        } else {
            let hue = Self.hue(name)
            ZStack {
                LinearGradient(colors: [Color(hue: hue, saturation: 0.55, brightness: 0.45), Color(hue: hue + 0.08, saturation: 0.6, brightness: 0.22)],
                               startPoint: .topLeading, endPoint: .bottomTrailing)
                Text(name).font(.system(size: 20, weight: .heavy, design: .rounded))
                    .multilineTextAlignment(.center).lineLimit(2).minimumScaleFactor(0.6)
                    .foregroundStyle(.white.opacity(0.92)).shadow(color: .black.opacity(0.4), radius: 2, y: 1)
                    .padding(12)
            }
        }
    }

    /// Stable per name (FNV-1a), 0..<1.
    static func hue(_ s: String) -> Double {
        var h: UInt32 = 2_166_136_261
        for b in s.utf8 { h = (h ^ UInt32(b)) &* 16_777_619 }
        return Double(h % 360) / 360
    }
}
