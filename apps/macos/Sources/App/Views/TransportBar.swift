// Transport controls + filmstrip timeline (FilmstripTimeline.swift) (essentials only; the rest lives in menus / "…").
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct HoldButton: View {
    let systemImage: String
    let help: String
    let onChange: (Bool) -> Void
    @State private var held = false

    var body: some View {
        Image(systemName: systemImage)
            .font(.system(size: 14, weight: .medium))
            .frame(width: 34, height: 26)
            .background(held ? Color.accentColor.opacity(0.35) : Color.secondary.opacity(0.12), in: RoundedRectangle(cornerRadius: 6))
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in if !held { held = true; onChange(true) } }
                .onEnded { _ in held = false; onChange(false) })
            .help(help)
    }
}

/// Plain icon button matching HoldButton's look.
struct IconButton: View {
    let systemImage: String
    let help: String
    var prominent = false
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: prominent ? 16 : 14, weight: .medium))
                .frame(width: prominent ? 40 : 34, height: 26)
                .background(Color.secondary.opacity(prominent ? 0.2 : 0.12), in: RoundedRectangle(cornerRadius: 6))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(help)
    }
}

/// The single record toggle: red glowing "Record" in record mode, gray in replay mode.
struct RecordToggleButton: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let st = model.status
        let on = st.recording && !st.practicing
        Button { model.toggleRecord() } label: {
            HStack(spacing: 6) {
                Circle().fill(on ? Color.red : Color.secondary.opacity(0.6)).frame(width: 9, height: 9)
                Text("Record").font(.system(size: 13, weight: .semibold))
            }
            .foregroundStyle(on ? Color.white : Color.secondary)
            .frame(width: 78, height: 26)
            .background(
                RoundedRectangle(cornerRadius: 13)
                    .fill(on ? Color.red.opacity(0.85) : Color.secondary.opacity(0.15))
            )
            // A steady glow: a forever-repeating pulse re-rendered the blurred shadow (and ran a
            // SwiftUI transaction on the main thread) every display refresh.
            .shadow(color: on ? Color.red.opacity(0.6) : .clear, radius: on ? 6 : 0)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(st.practicing)
        .help(on ? String(localized: "Record mode (click for playback mode: plays the recorded take)")
                 : String(localized: "Playback mode (click to return to record mode and continue recording from here)"))
    }
}

struct TransportBar: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        let st = model.status
        VStack(spacing: 6) {
            HStack(spacing: 10) {
                ClockView { Text(leftTime($0)).font(.system(.callout, design: .monospaced)) }
                FilmstripTimeline()
                ClockView { Text(rightTime($0)).font(.system(.callout, design: .monospaced)).foregroundStyle(.secondary) }
                TimelineSlotPicker()
            }
            HStack(spacing: 6) {
                RecordToggleButton()
                Divider().frame(height: 20).padding(.horizontal, 4)
                IconButton(systemImage: "backward.end.fill", help: st.practicing ? String(localized: "Back to A") : String(localized: "Go to Start")) { model.seek(to: 0) }
                HoldButton(systemImage: "backward.fill", help: String(localized: "Rewind while held (R2 / Delete)")) { model.setRewindHeld($0) }
                IconButton(systemImage: "backward.frame.fill", help: String(localized: "Step back one frame (while paused: D-pad ← / ,)")) { model.stepBack() }
                    .opacity(st.paused ? 1 : 0).disabled(!st.paused)
                IconButton(systemImage: st.paused ? "play.fill" : "pause.fill", help: String(localized: "Resume / Pause (R / Space)"), prominent: true) {
                    model.togglePause()
                }
                IconButton(systemImage: "forward.frame.fill", help: String(localized: "Frame advance (while paused: D-pad → / .)")) { model.frameAdvance() }
                    .opacity(st.paused ? 1 : 0).disabled(!st.paused)
                HoldButton(systemImage: "forward.fill", help: String(localized: "Fast-forward while held (L2 / Tab). Plays only the recorded range and stops at its end")) {
                    model.setFastForwardHeld($0)
                }
                .opacity(st.practicing ? 0.35 : 1).disabled(st.practicing)
                Button { model.toggleSlow() } label: {
                    Text(st.slow == .normal ? String(localized: "Slow") : String(localized: "Slow \(st.slow.label)"))
                        .font(.system(size: 12, weight: .medium))
                        .frame(minWidth: 58, minHeight: 26)
                        .background(st.slow == .normal ? Color.secondary.opacity(0.12) : Color.purple.opacity(0.35),
                                    in: RoundedRectangle(cornerRadius: 6))
                }
                .buttonStyle(.plain)
                .help("Slow 1/2 ⇔ normal speed (L / L key)")

                Spacer(minLength: 8)

                Button { model.togglePracticePanel() } label: {
                    Label(st.practicing ? String(localized: "Stop Practicing") : String(localized: "Practice"), systemImage: "repeat")
                        .font(.system(size: 12, weight: .medium))
                        .padding(.horizontal, 10).frame(height: 26)
                        .background(st.practicing || model.showPracticePanel ? Color.orange.opacity(0.4) : Color.secondary.opacity(0.12),
                                    in: RoundedRectangle(cornerRadius: 6))
                }
                .buttonStyle(.plain)
                .help("Practice mode (A/B repeat): practice a section as often as you like. Nothing is recorded (⇧⌘P)")

                ScaleToggle()

                Menu {
                    Button("Add Bookmark") { model.addBookmark() }.disabled(st.practicing)
                    Button("Back to Previous Take") { model.undoTake() }.disabled(st.undoDepth == 0 || st.practicing)
                    Button("Takes…") { openWindow(id: "takes") }
                    Divider()
                    Menu("Advance by Frames") {
                        ForEach([5, 10, 30, 60], id: \.self) { n in Button("\(n) frames") { model.frameAdvance(n) } }
                    }
                    Divider()
                    Button("Soft Reset") { model.softReset() }.disabled(!st.recording && !st.practicing)
                    Button("Power Cycle (Off and On)") { model.powerCycle() }.disabled(!st.recording && !st.practicing)
                    Divider()
                    Toggle("Show Latency", isOn: $model.showLatency)
                    Toggle("Sidebar", isOn: $model.showSidebar)
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .frame(width: 30)
                .help("More (bookmarks, takes, reset, …)")
            }
        }
        .padding(.horizontal, 14).padding(.vertical, 8)
    }

    private func leftTime(_ st: EmuStatus) -> String {
        st.practicing ? Engine.timecode(forFrame: st.practiceFrame) : Engine.timecode(forFrame: st.frame)
    }

    private func rightTime(_ st: EmuStatus) -> String {
        if st.practicing { return st.practiceLength > 0 ? Engine.timecode(forFrame: st.practiceLength) : "--:--.--" }
        return Engine.timecode(forFrame: st.takeLength) + (st.unsaved ? " •" : "")
    }
}

/// Content made from the per-frame status: observes AppModel.clock only (see AppModel.status).
struct ClockView<Content: View>: View {
    @ObservedObject private var clock = AppModel.shared.clock
    @ViewBuilder let content: (EmuStatus) -> Content
    var body: some View { content(clock.status) }
}

/// Pixel-perfect / FILL segmented control.
struct ScaleToggle: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        Picker("", selection: $model.integerScale) {
            Text("Integer").tag(true)
            Text("FILL").tag(false)
        }
        .pickerStyle(.segmented).labelsHidden().fixedSize()
        .help("Integer: largest integer scale that fits (sharp) / FILL: fill the window, aspect ratio kept (⌘F to toggle)")
    }
}
