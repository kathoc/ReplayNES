// UI language policy: Japanese only when the first preferred system language is Japanese.
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class UILanguageTests: XCTestCase {
    func testJapaneseFirstPicksJapanese() {
        XCTAssertEqual(UILanguage.choose(systemPreferred: ["ja-JP", "en-US"]), "ja")
        XCTAssertEqual(UILanguage.choose(systemPreferred: ["ja"]), "ja")
    }

    func testEverythingElsePicksEnglish() {
        XCTAssertEqual(UILanguage.choose(systemPreferred: ["fr-FR", "ja-JP", "en"]), "en")
        XCTAssertEqual(UILanguage.choose(systemPreferred: ["en-GB"]), "en")
        XCTAssertEqual(UILanguage.choose(systemPreferred: ["zh-Hans-CN"]), "en")
        XCTAssertEqual(UILanguage.choose(systemPreferred: []), "en")
    }
}
