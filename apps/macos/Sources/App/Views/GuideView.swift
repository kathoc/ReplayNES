// Controls Guide (Help menu): controller / keyboard layout, record toggle, practice mode.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct GuideView: View {
    private let pad: [(String, String)] = [
        (String(localized: "D-pad / Left Stick"), String(localized: "Move (while paused, D-pad ←/→ steps back / advances a frame)")),
        (String(localized: "Right / Bottom button"), String(localized: "NES A / B (Pro Controller A / B, Xbox B / A, PS ○ / ✕)")),
        (String(localized: "Top / Left button"), String(localized: "Turbo A / Turbo B (Pro Controller X / Y, Xbox Y / X, PS △ / □)")),
        (String(localized: "+ / − (Menu / Options)"), "START / SELECT"),
        (String(localized: "ZL / L2 / LT (hold)"), String(localized: "Rewind")),
        (String(localized: "ZR / R2 / RT (hold)"), String(localized: "Fast-forward (recorded range only; pauses at the end)")),
        ("R", String(localized: "Pause / Resume")),
        ("L", String(localized: "Slow 1/2 ⇔ normal speed")),
    ]
    private let keys: [(String, String)] = [
        (String(localized: "Arrow keys / X / Z"), String(localized: "Move / A / B")),
        (String(localized: "Return / Right Shift, \\"), "START / SELECT"),
        (String(localized: "Delete (hold)"), String(localized: "Rewind")),
        (String(localized: "Tab (hold)"), String(localized: "Fast-forward")),
        ("Space", String(localized: "Pause / Resume")),
        ("L", String(localized: "Slow 1/2 ⇔ normal speed")),
        (", / .", String(localized: "Step back / Frame advance")),
        ("B", String(localized: "Add Bookmark")),
        ("⇧⌘P", String(localized: "Practice panel (stops practicing while practicing)")),
        ("⇧⌘M", String(localized: "Toggle record / playback")),
        ("⌘F", String(localized: "Toggle Integer / FILL")),
    ]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                Text("Controls Guide").font(.title2.bold())
                section("Controller (defaults; can be changed in Settings)", pad)
                section("Keyboard (only while ReplayNES is in front)", keys)
                VStack(alignment: .leading, spacing: 6) {
                    Text("Record Button").font(.headline)
                    Text("A glowing red “Record” means record mode. Click it to switch to gray playback mode, which plays the recorded take (from the start if you are at the end). Click again to return to a paused state where you can continue recording from that position. If you rewind and then play, a new take branches off automatically and the old continuation is kept.")
                        .font(.callout).fixedSize(horizontal: false, vertical: true)
                }
                VStack(alignment: .leading, spacing: 6) {
                    Text("Practice Mode (A/B Repeat)").font(.headline)
                    Text("Open the section panel with the “Practice” button, press “A” at the start of the section and “B” at the end you reach by playing on from A. Press ▶︎ to practice that section repeatedly. At B it pauses for 0.5 s, then rewinds to A and starts again. Nothing is recorded while practicing; “Stop Practicing” returns to the original position in the take. Up to 8 sections are saved in the project.")
                        .font(.callout).fixedSize(horizontal: false, vertical: true)
                }
            }
            .padding(20)
        }
        .frame(minWidth: 520, minHeight: 480)
    }

    private func section(_ title: LocalizedStringKey, _ rows: [(String, String)]) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.headline)
            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 4) {
                ForEach(rows, id: \.0) { r in
                    GridRow {
                        Text(r.0).font(.system(.callout, design: .monospaced))
                        Text(r.1).font(.callout).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }
}
