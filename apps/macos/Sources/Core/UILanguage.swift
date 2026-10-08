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

    /// Settings › System › Language: "auto" (the rule above), "ja" or "en". Applied at launch.
    static var preference: String {
        get { UserDefaults.standard.string(forKey: "uiLanguage") ?? "auto" }
        set { UserDefaults.standard.set(newValue, forKey: "uiLanguage") }
    }

    /// The language's own name ("English", and Japanese written in Japanese).
    static func nativeName(_ code: String) -> String {
        Locale(identifier: code).localizedString(forLanguageCode: code) ?? code
    }

    /// Must run before any localized string is resolved (App.init).
    static func apply() {
        let pref = preference
        let lang = pref == "ja" || pref == "en" ? pref : choose(systemPreferred: systemLanguages())
        let defaults = UserDefaults.standard
        if defaults.persistentDomain(forName: Bundle.main.bundleIdentifier ?? "")?["AppleLanguages"] as? [String] != [lang] {
            defaults.set([lang], forKey: "AppleLanguages")
        }
        syncCore()
    }

    /// The user's system-wide language list (global domain). `UserDefaults(suiteName:
    /// UserDefaults.globalDomain)` returns nil, and `UserDefaults.standard` would see the value this
    /// app pins in its own domain, so read the global domain through CFPreferences.
    static func systemLanguages() -> [String] {
        CFPreferencesCopyValue("AppleLanguages" as CFString, kCFPreferencesAnyApplication,
                               kCFPreferencesCurrentUser, kCFPreferencesAnyHost) as? [String] ?? []
    }

    /// The shared frontend core resolves its strings in the language the bundle uses.
    static func syncCore() {
        rnf_l10n_set_language(Bundle.main.preferredLocalizations.first?.lowercased().hasPrefix("ja") == true ? "ja" : "en")
    }
}
