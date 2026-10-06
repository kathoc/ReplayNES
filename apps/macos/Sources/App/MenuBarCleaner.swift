// Keeps the menu bar compact: AppKit / SwiftUI insert items that make no sense in a game
// recorder (empty フォーマット menu, 作文ツール / 自動入力 in 編集). They are removed whenever menus
// change. Menu order stays the standard one (ファイル 編集 表示 再生 ウインドウ ヘルプ): moving
// SwiftUI-managed menus breaks their updates. Only the main menu bar is touched.
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit

final class MenuBarCleaner {
    static let shared = MenuBarCleaner()
    private var scheduled = false
    private var cleaning = false
    /// Items added to the main menu since launch (diagnostics: stays flat while playing).
    private(set) var mainMenuAdds = 0

    /// Edit-menu actions text fields need (rename practice slots / bookmarks).
    private static let editActions: Set<Selector> = [
        #selector(NSText.cut(_:)), #selector(NSText.copy(_:)), #selector(NSText.paste(_:)),
        #selector(NSText.delete(_:)), #selector(NSText.selectAll(_:)),
    ]

    func start() {
        NotificationCenter.default.addObserver(forName: NSMenu.didAddItemNotification, object: nil, queue: .main) { [weak self] n in
            guard let self, !self.cleaning, let m = n.object as? NSMenu, self.belongsToMainMenu(m) else { return }
            self.mainMenuAdds += 1
            self.schedule()
        }
        schedule()
    }

    private func belongsToMainMenu(_ m: NSMenu) -> Bool {
        var cur: NSMenu? = m
        while let c = cur {
            if c === NSApp.mainMenu { return true }
            cur = c.supermenu
        }
        return false
    }

    private func schedule() {
        guard !scheduled else { return }
        scheduled = true
        DispatchQueue.main.async { [weak self] in
            self?.scheduled = false
            self?.clean()
        }
    }

    func clean() {
        guard let bar = NSApp.mainMenu else { return }
        cleaning = true
        defer { cleaning = false }
        for item in bar.items.dropFirst() { // never the application menu
            guard let sub = item.submenu else { continue }
            if sub.items.contains(where: { $0.action == #selector(NSText.paste(_:)) }) {
                // 編集: keep only cut/copy/paste/delete/select all.
                for i in sub.items.reversed() where !i.isSeparatorItem && !(i.action.map(Self.editActions.contains) ?? false) {
                    sub.removeItem(i)
                }
                while let f = sub.items.first, f.isSeparatorItem { sub.removeItem(f) }
            }
        }
        // The empty フォーマット menu (its only group is replaced with nothing). Other menus may be
        // populated lazily by SwiftUI, so only this one is removed, and only while empty.
        for item in bar.items.dropFirst().reversed() where ["フォーマット", "Format"].contains(item.title) {
            if let sub = item.submenu, !sub.items.contains(where: { !$0.isSeparatorItem && !$0.isHidden }) {
                bar.removeItem(item)
            }
        }
    }
}
