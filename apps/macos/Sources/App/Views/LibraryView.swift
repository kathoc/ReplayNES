// Library = the start screen (docs/design/UI_REDESIGN.md, "Library"): a large "Continue" card (the
// last session - also after a crash - else the latest project), a row of filter chips (All /
// Favorites / Recent), the sort and the search, then big game cards (4 per row, paged with L / R
// instead of scrolling; the title from the game database in the UI language, "maker · year" under
// it, a star on favourites), or one card saying where to put ROMs. ROMs live in
// ~/Documents/ReplayNES/ROM, their projects (matched by SHA-256) in Projects. Controller / keyboard:
// D-pad moves, A plays, Y = favourite, X = projects of the game, View / S = next sort, / = search.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Focus and paging of the library (main thread).
final class LibraryNav: ObservableObject {
    static let heroFocus = -1
    static let barFocus = -3
    /// The filter / sort / search row: filters, then the sort, then the search.
    static let barItems = Int(RNF_LIBRARY_FILTER_COUNT.rawValue) + 2
    static var barSort: Int { Int(RNF_LIBRARY_FILTER_COUNT.rawValue) }
    static var barSearch: Int { barSort + 1 }

    /// -1 = the Continue card, -3 = the filter / sort / search row, else a game (all pages).
    @Published var focus = 0
    @Published var barFocus = 0
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
        library.arranged(query: search.trimmingCharacters(in: .whitespaces))
    }

    func pageCount(_ n: Int) -> Int { QuickMenuNav.pageCount(items: n, perPage: perPage) }

    /// A filter chip, the sort (cycles) or the search.
    func barAction(_ item: Int, library: LibraryModel) {
        if item < Self.barSort {
            library.filter = rnf_library_filter(rawValue: UInt32(item))
            page = 0
        } else if item == Self.barSort {
            library.sort = rnf_library_sort(rawValue: (library.sort.rawValue + 1) % RNF_LIBRARY_SORT_COUNT.rawValue)
            page = 0
        } else {
            searchRequested += 1
        }
    }

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
        if library.roms.isEmpty {
            // The Continue card (if any) and the "where to put ROMs" card.
            switch n {
            case .up: if hero { focus = Self.heroFocus }
            case .down: focus = 0
            case .confirm: if focus < 0 && hero { LibraryHome.continueItem(m)?.run() } else { library.revealROMFolder() }
            case .back, .escape: if m.status.hasSession { m.hideLibraryScreen() }
            default: break
            }
            return
        }
        let range = QuickMenuNav.pageRange(items: list.count, perPage: perPage, page: page)
        let local = focus - range.lowerBound
        let firstCard: Int? = range.isEmpty ? nil : range.lowerBound
        switch n {
        case .up:
            // Top to bottom: the filter / sort / search row, the Continue card, the game cards.
            if focus == Self.barFocus { return }
            if focus == Self.heroFocus { focus = Self.barFocus; return }
            if local < columns { focus = hero ? Self.heroFocus : Self.barFocus } else { focus -= columns }
        case .down:
            if focus == Self.barFocus { focus = hero ? Self.heroFocus : (firstCard ?? Self.barFocus); return }
            if focus == Self.heroFocus { if let f = firstCard { focus = f }; return }
            if let j = QuickMenuNav.move(local, count: range.count, columns: columns, .down) { focus = range.lowerBound + j }
        case .left, .right:
            if focus == Self.barFocus {
                barFocus = max(0, min(Self.barItems - 1, barFocus + (n == .left ? -1 : 1)))
                return
            }
            guard focus >= 0 else { return }
            let d: NavDirection = n == .left ? .left : .right
            if let j = QuickMenuNav.move(local, count: range.count, columns: columns, d) { focus = range.lowerBound + j }
            else { flip(n == .left ? -1 : 1, count: list.count) }
        case .pagePrev, .pageNext:
            flip(n == .pagePrev ? -1 : 1, count: list.count)
        case .confirm:
            if focus == Self.heroFocus { LibraryHome.continueItem(m)?.run() }
            else if focus == Self.barFocus { barAction(barFocus, library: library) }
            else if list.indices.contains(focus) { LibraryHome.play(list[focus], model: m) }
        case .x:
            if focus >= 0, list.indices.contains(focus) { projectsFocus = 0; projectsOf = list[focus] }
        case .y:
            if focus >= 0, list.indices.contains(focus) { library.toggleFavorite(list[focus]) }  // the focus stays
        case .options:
            barAction(Self.barSort, library: library)
        case .search:
            searchRequested += 1
        case .back, .escape:
            if !search.isEmpty { search = ""; focus = 0; page = 0 }
            else if m.status.hasSession { m.hideLibraryScreen() }
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
        if page >= pages { page = max(0, pages - 1) }
        if focus == Self.barFocus { return }
        if count == 0 { focus = hero ? Self.heroFocus : Self.barFocus; return }
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
    static let heroHeight: CGFloat = 140

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
                if library.roms.isEmpty && hero == nil {
                    emptyCard.frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    if let hero { heroCard(hero) }
                    if library.roms.isEmpty {
                        emptyCard.frame(maxWidth: .infinity)
                    } else {
                        grid(list, cardW: cardW, cardH: cardH, perPage: per)
                    }
                    Spacer(minLength: 0)
                    footer(list)
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
        .onChange(of: nav.searchRequested) { _, _ in nav.focus = LibraryNav.barFocus; nav.barFocus = LibraryNav.barSearch; searchFocused = true }
    }

    // MARK: header / footer

    private var header: some View {
        HStack(spacing: 12) {
            (Text(verbatim: "Replay") + Text(verbatim: "NES").foregroundColor(QMStyle.brand)).font(.system(size: 22, weight: .bold))
            if library.scanning { ProgressView().controlSize(.small) }
            Spacer(minLength: 12)
            if !library.roms.isEmpty { toolbar }
        }
        .frame(height: 34)
    }

    /// Filter chips, the sort and the search (reached with up from the cards).
    private var toolbar: some View {
        let _ = library.catalogRevision
        let icons = ["square.grid.2x2", "star.fill", "clock.arrow.circlepath"]
        return HStack(spacing: 8) {
            ForEach(0..<LibraryCatalog.filters.count, id: \.self) { i in
                chip(i, icon: icons[i], text: LibraryCatalog.name(LibraryCatalog.filters[i]),
                     selected: library.filter == LibraryCatalog.filters[i])
            }
            chip(LibraryNav.barSort, icon: "arrow.up.arrow.down", text: LibraryCatalog.name(library.sort), selected: false)
                .padding(.leading, 10)
            searchField
        }
    }

    private func chip(_ i: Int, icon: String, text: String, selected: Bool) -> some View {
        let focused = nav.focus == LibraryNav.barFocus && nav.barFocus == i
        return HStack(spacing: 5) {
            Image(systemName: icon).font(.system(size: 11, weight: .semibold))
            Text(text).font(.system(size: 13, weight: .semibold)).lineLimit(1)
        }
        .foregroundStyle(.white.opacity(selected || focused ? 1 : 0.7))
        .padding(.horizontal, 12).frame(height: 30)
        .background(Capsule().fill(selected ? QMStyle.brand : (focused ? QMStyle.tileFocused : QMStyle.tile)))
        .overlay(Capsule().strokeBorder(focused ? QMStyle.brand : .clear, lineWidth: 2))
        .fixedSize()
        .contentShape(Capsule())
        .onHover { if $0 { nav.focus = LibraryNav.barFocus; nav.barFocus = i } }
        .onTapGesture { nav.focus = LibraryNav.barFocus; nav.barFocus = i; nav.barAction(i, library: library) }
        .animation(QMStyle.anim, value: focused)
    }

    private var searchField: some View {
        let focused = nav.focus == LibraryNav.barFocus && nav.barFocus == LibraryNav.barSearch
        return HStack(spacing: 6) {
            Image(systemName: "magnifyingglass").foregroundStyle(.white.opacity(0.5))
            TextField("Search", text: $nav.search)
                .textFieldStyle(.plain).frame(width: 130)
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
        .overlay(Capsule().strokeBorder(focused && !searchFocused ? QMStyle.brand : .clear, lineWidth: 2))
        .onHover { if $0 { nav.focus = LibraryNav.barFocus; nav.barFocus = LibraryNav.barSearch } }
    }

    /// The View / Select button of the controller in use ("S" on the keyboard).
    private func optionsGlyph() -> String {
        guard let c = monitor.controllers.min(by: { $0.slot < $1.slot }) else { return "S" }
        let l = c.family.label("options")
        return l.isEmpty ? "⧉" : l
    }

    private func footer(_ list: [LibraryROM]) -> some View {
        let g = HintGlyphs.current(monitor.controllers)
        let count = list.count
        let pages = nav.pageCount(count)
        let focusedRom: LibraryROM? = nav.focus >= 0 && list.indices.contains(nav.focus) ? list[nav.focus] : nil
        let description: String = {
            if nav.focus == LibraryNav.heroFocus { return String(localized: "Pick up where you left off") }
            if nav.focus == LibraryNav.barFocus {
                if nav.barFocus < LibraryNav.barSort { return LibraryCatalog.name(LibraryCatalog.filters[nav.barFocus]) }
                if nav.barFocus == LibraryNav.barSort { return String(localized: "Sort by \(LibraryCatalog.name(library.sort))") }
                return String(localized: "Search ROMs")
            }
            guard let r = focusedRom else { return "" }
            return r.details.isEmpty ? r.relativePath : r.details
        }()
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
            Text(description).font(QMStyle.hint).foregroundStyle(.white.opacity(0.6)).lineLimit(1)
            Spacer()
            if nav.focus == LibraryNav.barFocus {
                hint(g.confirm, String(localized: "Select"))
            } else if let r = focusedRom {
                hint(g.confirm, String(localized: "Play"))
                hint(g.y, library.isFavorite(r) ? String(localized: "Unfavorite") : String(localized: "Favorite"))
                hint(g.x, String(localized: "Projects"))
            }
            hint(optionsGlyph(), LibraryCatalog.name(library.sort))
                .onTapGesture { nav.barAction(LibraryNav.barSort, library: library) }
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

    /// The game open now, else the last session (resume record: also after a crash or force quit),
    /// else the latest library project.
    static func continueItem(_ m: AppModel) -> ContinueItem? {
        if m.status.hasSession {
            let rom = m.library.rom(sha256: m.current?.romSHA256)?.title
                ?? GameInfo.title(fileName: URL(fileURLWithPath: m.status.romPath).lastPathComponent)
            return ContinueItem(title: rom, subtitle: String(localized: "Playing now"),
                                image: ThumbnailStore.image(project: m.status.projectPath) ?? ThumbnailStore.image(romSHA: m.current?.romSHA256),
                                run: { m.hideLibraryScreen() })
        }
        if let r = m.pendingResume {
            let title = m.library.rom(sha256: r.romSHA256)?.title
                ?? GameInfo.title(fileName: URL(fileURLWithPath: r.romPath.isEmpty ? r.projectPath : r.romPath).lastPathComponent)
            var sub = r.updated.formatted(.relative(presentation: .named))
            if r.isTemp { sub += String(localized: " · ") + String(localized: "Not saved as a project") }
            return ContinueItem(title: title, subtitle: sub,
                                image: ThumbnailStore.image(project: r.projectPath) ?? ThumbnailStore.image(romSHA: r.romSHA256),
                                run: { m.continueLast() })
        }
        let all = m.library.projectsBySHA.values.flatMap { $0 }
        guard let p = all.max(by: { $0.modified < $1.modified }) else { return nil }
        let name = m.library.rom(sha256: p.romSHA256)?.title ?? GameInfo.title(fileName: p.romName.isEmpty ? p.name : p.romName)
        return ContinueItem(title: name,
                            subtitle: p.modified.formatted(.relative(presentation: .named)),
                            image: ThumbnailStore.image(project: p.url.path) ?? ThumbnailStore.image(romSHA: p.romSHA256),
                            run: { m.continueProject(p.url) })
    }

    private func heroCard(_ c: ContinueItem) -> some View {
        let focused = nav.focus == LibraryNav.heroFocus
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
        .onHover { if $0 { nav.focus = LibraryNav.heroFocus } }
        .onTapGesture { nav.focus = LibraryNav.heroFocus; c.run() }
        .animation(QMStyle.anim, value: focused)
    }

    // MARK: game cards

    private func grid(_ list: [LibraryROM], cardW: CGFloat, cardH: CGFloat, perPage: Int) -> some View {
        let range = QuickMenuNav.pageRange(items: list.count, perPage: perPage, page: nav.page)
        let idx = Array(range)
        let rows = stride(from: 0, to: idx.count, by: 4).map { Array(idx[$0..<min(idx.count, $0 + 4)]) }
        let empty: String = !nav.search.isEmpty ? String(localized: "No games match “\(nav.search)”")
            : library.filter == RNF_LIBRARY_FILTER_FAVORITES ? String(localized: "No favorites yet") : String(localized: "Nothing played yet")
        return VStack(alignment: .leading, spacing: Self.gap) {
            if list.isEmpty {
                Text(empty).foregroundStyle(.white.opacity(0.6)).frame(height: cardH)
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

    static func playTime(_ seconds: Double) -> String {
        let m = max(1, Int(seconds / 60))
        return m < 60 ? String(localized: "\(m) min") : String(localized: "\(m / 60) h \(m % 60) min")
    }

    private func gameCard(_ rom: LibraryROM, index i: Int, width: CGFloat, artHeight: CGFloat) -> some View {
        let focused = nav.focus == i
        let projects = library.projects(for: rom)
        let title = rom.title
        var sub = rom.byline
        if library.filter == RNF_LIBRARY_FILTER_RECENT || library.sort == RNF_LIBRARY_SORT_RECENT,
           let h = library.catalog.history(rom.sha256) {
            sub = h.lastPlayed.formatted(date: .abbreviated, time: .shortened) + String(localized: " · ") + Self.playTime(h.playSeconds)
        }
        if sub.isEmpty { sub = projects.isEmpty ? String(localized: "New") : projects[0].modified.formatted(.relative(presentation: .named)) }
        let favorite = library.isFavorite(rom)
        return VStack(alignment: .leading, spacing: 0) {
            GameArt(image: ThumbnailStore.image(romSHA: rom.sha256), name: title)
                .frame(width: width, height: artHeight)
                .clipped()
                .overlay(alignment: .topTrailing) {
                    if favorite {
                        Image(systemName: "star.fill").font(.system(size: 13, weight: .bold)).foregroundStyle(.yellow)
                            .frame(width: 26, height: 26).background(Circle().fill(Color.black.opacity(0.65))).padding(6)
                    }
                }
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.system(size: 15, weight: .semibold)).lineLimit(1)
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
            Button(favorite ? "Unfavorite" : "Favorite") { library.toggleFavorite(rom) }
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
                Text(rom.title).font(QMStyle.title).lineLimit(1)
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
