// UI language policy: Japanese when the system's first preferred language is Japanese, English
// otherwise. Standard bundle matching would pick Japanese for e.g. [fr, ja, en]; this pins the
// app to exactly one of its two localizations, re-evaluated at every launch.
// An explicit `-AppleLanguages` launch argument (argument domain) still wins.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum UILanguage {
    static func choose(systemPreferred: [String]) -> String {
        systemPreferred.first?.lowercased().hasPrefix("ja") == true ? "ja" : "en"
    }

    /// Must run before any localized string is resolved (App.init).
    static func apply() {
        let global = UserDefaults(suiteName: UserDefaults.globalDomain)?.stringArray(forKey: "AppleLanguages") ?? []
        let lang = choose(systemPreferred: global)
        let defaults = UserDefaults.standard
        if defaults.persistentDomain(forName: Bundle.main.bundleIdentifier ?? "")?["AppleLanguages"] as? [String] != [lang] {
            defaults.set([lang], forKey: "AppleLanguages")
        }
    }
}
