// "Reset Project…": the current project starts over from power-on with an empty timeline (same
// ROM, same project location), after a confirmation. A saved project is first backed up: a copy
// of its folder goes to the Trash (ProjectBackup). The engine reset (rn_session_reset) is saved
// at once and crash-safe; the filmstrip cache, the emulation state and the resume record restart
// like for a new project.
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit

extension EmulationController {
    /// Emulation thread. `backup`: copy the project to the Trash first (saved projects). Returns
    /// an error message, or nil when the project was reset.
    func resetProject(keepPracticeSlots: Bool, backup: Bool) -> String? {
        guard let s = session else { return nil }
        if s.mode == RN_MODE_PRACTICE {
            do { try s.setMode(RN_MODE_RECORD) } catch { return (error as? LocalizedError)?.errorDescription ?? "\(error)" }
        }
        if fastForwardActive { endFastForward(s) }
        if backup, !s.projectDir.isEmpty {
            do {
                // The journal holds everything up to now, so the copy reopens with all of it.
                if s.hasUnsavedChanges { try s.autosave() }
                try ProjectBackup.makeBackup(of: URL(fileURLWithPath: s.projectDir))
            } catch {
                return String(localized: "Couldn’t make a backup copy, so the project was not reset.") + "\n\n"
                    + ((error as? LocalizedError)?.errorDescription ?? "\(error)")
            }
        }
        do {
            try s.reset(keepPracticeSlots: keepPracticeSlots)
        } catch {
            // Not committed: the engine restored the previous content.
            paused = true
            markStructureDirty()
            publishVideo()
            return (error as? LocalizedError)?.errorDescription ?? "\(error)"
        }
        install(s)  // like a new project: empty take at frame 0, record mode, running
        return nil
    }
}

extension AppModel {
    /// File ▸ Reset Project… (also in the sidebar and the Takes window).
    func resetProjectPrompt() {
        guard status.hasSession, let c = current else { return }
        let dir = status.projectPath
        let saved = !dir.isEmpty && !c.isTemp
        let a = NSAlert()
        a.alertStyle = .warning
        a.messageText = String(localized: "Reset this project?")
        var info = String(localized: "The recording starts over from power-on: every take, bookmark and the take history are deleted. The ROM and the project location stay the same.")
        if saved {
            info += "\n\n" + String(localized: "A backup copy of the project is moved to the Trash first, so you can still recover it from there.")
        } else {
            info += "\n\n" + String(localized: "This session isn’t saved as a project, so no backup is made.")
        }
        a.informativeText = info
        let keep = NSButton(checkboxWithTitle: String(localized: "Keep A/B repeat sections"), target: nil, action: nil)
        keep.state = .on
        a.accessoryView = keep
        let ok = a.addButton(withTitle: String(localized: "Reset"))
        ok.hasDestructiveAction = true
        a.addButton(withTitle: String(localized: "Cancel"))
        guard a.runModal() == .alertFirstButtonReturn else { return }
        performProjectReset(keepPracticeSlots: keep.state == .on, backup: saved)
    }

    /// Without the dialog (also the `resetProject` test action).
    func performProjectReset(keepPracticeSlots: Bool, backup: Bool) {
        let err: String?? = emu.sync(timeout: 120) { e in e.resetProject(keepPracticeSlots: keepPracticeSlots, backup: backup) }
        switch err {
        case .some(nil):
            sessionWasReset(keepPracticeSlots: keepPracticeSlots)
            flash(backup ? String(localized: "Project reset (a backup is in the Trash)") : String(localized: "Project reset"))
        case .some(.some(let msg)):
            showError(String(localized: "Couldn’t reset the project"), msg)
        case .none:
            showError(String(localized: "Couldn’t reset the project"), String(localized: "Saving didn’t respond"))
        }
    }
}
