// The Swift bridge of the core's game database and library catalog (Sources/Core/Library.swift):
// lookups, localized display strings, favourites / history persistence and the library order.
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class GameDatabaseTests: XCTestCase {
    private var tmp: URL!
    private var savedLanguage = "en"

    override func setUpWithError() throws {
        tmp = FileManager.default.temporaryDirectory.appendingPathComponent("rn-gamedb-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
        savedLanguage = String(cString: rnf_l10n_language())
    }

    override func tearDownWithError() throws {
        rnf_l10n_set_language(savedLanguage)
        try? FileManager.default.removeItem(at: tmp)
    }

    private func rom(_ name: String, sha: String?, game: GameInfo?) -> LibraryROM {
        LibraryROM(url: tmp.appendingPathComponent(name), name: name, relativePath: name, size: 0, modified: Date(), sha256: sha, game: game)
    }

    func testLookupByHashAndFileName() throws {
        let smb = try XCTUnwrap(GameInfo.find(crc32: 0xD445_F698))  // Super Mario Bros. PRG+CHR (Nestopia's hash)
        XCTAssertEqual(smb.year, 1985)
        XCTAssertEqual(GameInfo.find(fileName: "Super Mario Bros. (World).nes"), smb)
        XCTAssertEqual(GameInfo.find(id: smb.id), smb)
        XCTAssertNil(GameInfo.find(fileName: "Definitely Not A Game (PD).nes"))

        rnf_l10n_set_language("ja")
        let r = rom("Super Mario Bros. (World)", sha: "aa", game: smb)
        XCTAssertEqual(r.title, "スーパーマリオブラザーズ")
        XCTAssertEqual(r.byline, "任天堂・1985")
        XCTAssertEqual(r.details, "任天堂・1985・アクション")
        let front = try XCTUnwrap(GameInfo.find(fileName: "Front Line (Japan).nes"))
        XCTAssertEqual(String(cString: front.info.reading), "フロントライン")
        rnf_l10n_set_language("en")
        XCTAssertEqual(r.title, "Super Mario Bros.")
        XCTAssertEqual(rom("Unknown Thing (Japan) [!]", sha: nil, game: nil).title, "Unknown Thing")
    }

    func testCatalogOrderFavoritesAndHistoryPersist() throws {
        rnf_l10n_set_language("ja")
        let gradius = try XCTUnwrap(GameInfo.find(fileName: "Gradius (Japan).nes"))
        let smb = try XCTUnwrap(GameInfo.find(crc32: 0xD445_F698))
        let roms = [rom("zz homebrew", sha: "k3", game: nil), rom("Super Mario Bros.", sha: "k2", game: smb),
                    rom("Gradius (Japan)", sha: "k1", game: gradius)]
        let file = tmp.appendingPathComponent("library.json")
        let c = LibraryCatalog()
        c.load(file)
        // Japanese: gojūon order of the readings (グ < ス), unknown ROMs after.
        XCTAssertEqual(c.arrange(roms, query: "").map(\.sha256), ["k1", "k2", "k3"])
        XCTAssertEqual(c.arrange(roms, query: "まりお").map(\.sha256), ["k2"])

        XCTAssertTrue(c.toggleFavorite("k2"))
        XCTAssertFalse(c.toggleFavorite(nil))
        c.filter = RNF_LIBRARY_FILTER_FAVORITES
        XCTAssertEqual(c.arrange(roms, query: "").map(\.sha256), ["k2"])

        c.recordPlay("k3", start: Date(timeIntervalSince1970: 1000), seconds: 30)
        c.recordPlay("k1", start: Date(timeIntervalSince1970: 2000), seconds: 90)
        c.filter = RNF_LIBRARY_FILTER_RECENT
        XCTAssertEqual(c.arrange(roms, query: "").map(\.sha256), ["k1", "k3"])
        c.sort = RNF_LIBRARY_SORT_YEAR

        let d = LibraryCatalog()
        d.load(file)
        XCTAssertTrue(d.isFavorite("k2"))
        XCTAssertEqual(d.filter, RNF_LIBRARY_FILTER_RECENT)
        XCTAssertEqual(d.sort, RNF_LIBRARY_SORT_YEAR)
        XCTAssertEqual(d.history("k1"), LibraryCatalog.History(lastPlayed: Date(timeIntervalSince1970: 2000), playSeconds: 90, plays: 1))
        XCTAssertEqual(LibraryCatalog.name(RNF_LIBRARY_SORT_PUBLISHER), "メーカー")
        XCTAssertEqual(LibraryCatalog.sorts.count, 5)
        XCTAssertEqual(LibraryCatalog.filters.count, 3)
    }
}
