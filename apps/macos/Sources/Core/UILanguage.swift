// UI language policy: Japanese when the system's first preferred language is Japanese, English
// otherwise. Standard bundle matching would pick Japanese for e.g. [fr, ja, en]; this pins the
// app to exactly one of its two localizations, re-evaluated at every launch.
// An explicit `-AppleLanguages` launch argument (argument domain) still wins. The rule itself lives
// in the shared frontend core (rnf_ui_language_choose).
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum UILanguage {
    static func choose(systemPreferred: [String]) -> String {
        withCStrings(systemPreferred) { String(cString: rnf_ui_language_choose($0, $0.count)) }
    }

    /// Must run before any localized string is resolved (App.init).
    static func apply() {
        let global = UserDefaults(suiteName: UserDefaults.globalDomain)?.stringArray(forKey: "AppleLanguages") ?? []
        let lang = choose(systemPreferred: global)
        let defaults = UserDefaults.standard
        if defaults.persistentDomain(forName: Bundle.main.bundleIdentifier ?? "")?["AppleLanguages"] as? [String] != [lang] {
            defaults.set([lang], forKey: "AppleLanguages")
        }
        syncCore()
    }

    /// The shared frontend core resolves its strings in the language the bundle uses.
    static func syncCore() {
        rnf_l10n_set_language(Bundle.main.preferredLocalizations.first?.lowercased().hasPrefix("ja") == true ? "ja" : "en")
    }
}
