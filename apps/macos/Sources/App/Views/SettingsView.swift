// Settings: controller diagram, key/controller remap, hotkeys, turbo/SOCD, display/audio,
// general, updates.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct SettingsView: View {
    /// Selected tab; the sidebar's "Button Layout…" sets "controller" before opening Settings.
    @AppStorage(SettingsView.tabKey) private var tab = "controller"
    static let tabKey = "settingsTab"

    var body: some View {
        TabView(selection: $tab) {
            ControllerTab().tabItem { Label("Controller", systemImage: "gamecontroller") }.tag("controller")
            BindingsTab(groups: [.player1, .player2]).tabItem { Label("Game Input", systemImage: "keyboard") }.tag("game")
            BindingsTab(groups: [.hotkey]).tabItem { Label("Hotkeys", systemImage: "command") }.tag("hotkey")
            TurboTab().tabItem { Label("Turbo & SOCD", systemImage: "bolt") }.tag("turbo")
            DisplayTab().tabItem { Label("Display & Audio", systemImage: "display") }.tag("display")
            UpdatesTab().tabItem { Label("Updates", systemImage: "arrow.triangle.2.circlepath") }.tag("updates")
        }
        .frame(width: 620, height: 520)
    }
}

/// Controller diagram: click a button on the picture to assign it; held buttons light up.
struct ControllerTab: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        ControllerTabContent(monitor: model.input.controllerMonitor)
    }
}

private struct ControllerTabContent: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var monitor: ControllerMonitor
    @AppStorage("diagramSlot") private var slot = 0
    @AppStorage("diagramFamily") private var familyChoice = "auto"

    var body: some View {
        let info = monitor.controllers.first { $0.slot == slot }
        let family = ControllerFamily(rawValue: familyChoice) ?? info?.family ?? .generic
        let slotCount = max(2, (monitor.controllers.map(\.slot).max() ?? 0) + 1)
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Picker("", selection: $slot) {
                    ForEach(0..<slotCount, id: \.self) { i in
                        let c = monitor.controllers.first { $0.slot == i }
                        let player = i == 0 ? "1P" : i == 1 ? "2P" : "—"
                        let name = c?.name ?? String(localized: "Not Connected")
                        Text("Pad \(i + 1) (\(player)): \(name)").tag(i)
                    }
                }
                .labelsHidden().frame(width: 300)
                Spacer()
                Picker("Layout", selection: $familyChoice) {
                    Text("Automatic").tag("auto")
                    ForEach(ControllerFamily.allCases) { Text($0.title).tag($0.rawValue) }
                }
                .frame(width: 250)
            }
            Text(statusText(info))
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            ControllerDiagramView(family: family, slot: slot, config: model.inputConfig, labels: info?.labels ?? [:],
                                  pressed: monitor.pressed) { id, action in
                model.input.setAssignment(id, action: action)
            }
            .frame(maxWidth: .infinity)
            HStack(spacing: 14) {
                legend(.blue.opacity(0.85), "Game input (recorded)")
                legend(.orange, "Hotkeys (not recorded)")
            }
            .font(.caption)
            Spacer(minLength: 0)
            HStack(alignment: .bottom) {
                Text("To assign by pressing a key or button, use the Game Input and Hotkeys tabs")
                    .font(.caption2).foregroundStyle(.secondary)
                Spacer()
                Button("Reset Pad \(slot + 1) to Defaults") { model.input.resetController(slot: slot) }
            }
        }
        .padding()
        .onAppear { monitor.setLive(true) }
        .onDisappear { monitor.setLive(false) }
    }

    private func statusText(_ info: ControllerInfo?) -> String {
        guard let info else {
            return String(localized: "Not connected (once connected, pressed buttons light up on the picture) · Click a button on the picture to change its assignment")
        }
        let kind = info.productCategory.isEmpty ? info.family.title : info.productCategory
        return String(localized: "\(info.name) (\(kind)) · Pressed buttons light up · Click a button on the picture to change its assignment")
    }

    private func legend(_ color: Color, _ text: LocalizedStringKey) -> some View {
        HStack(spacing: 4) {
            Capsule().fill(color).frame(width: 18, height: 10)
            Text(text).foregroundStyle(.secondary)
        }
    }
}

struct BindingsTab: View {
    @EnvironmentObject var model: AppModel
    let groups: [InputAction.Group]
    @State private var group: InputAction.Group?

    var body: some View {
        let g = group ?? groups[0]
        VStack(alignment: .leading, spacing: 8) {
            if groups.count > 1 {
                Picker("", selection: Binding(get: { g }, set: { group = $0 })) {
                    ForEach(groups, id: \.self) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented).labelsHidden()
            } else {
                Text("Hotkeys are handled separately from game input and are not recorded. They also work while paused. Controllers work even when ReplayNES isn’t in front (for example while you operate OBS).")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("While paused, the controller’s D-pad ←/→ steps back / advances one frame (hold to repeat)", isOn: $model.dpadStepWhenPaused)
                    .font(.caption)
            }
            List {
                ForEach(InputCatalog.allActions.filter { $0.group == g }) { action in
                    HStack(alignment: .firstTextBaseline) {
                        Text(action.label).frame(width: 200, alignment: .leading)
                        FlowChips(action: action)
                        Spacer()
                        if model.capturingAction == action.id {
                            Text("Press a key/button… (Esc to cancel)").font(.caption).foregroundStyle(.orange)
                        } else {
                            Button("Add…") { capture(action.id) }.controlSize(.small)
                        }
                    }
                }
            }
            HStack {
                Text("Settings file: ~/Library/Application Support/ReplayNES/bindings.json")
                    .font(.caption2).foregroundStyle(.secondary)
                Spacer()
                Button("Reset to Defaults") { model.input.resetToDefaults() }
            }
        }
        .padding()
    }

    private func capture(_ action: String) {
        model.capturingAction = action
        model.input.captureHandler = { id in
            model.capturingAction = nil
            if let id { model.input.bind(id, to: action) }
        }
    }
}

struct FlowChips: View {
    @EnvironmentObject var model: AppModel
    let action: InputAction
    var body: some View {
        let ids = model.inputConfig.inputs(for: action.id).sorted()
        HStack(spacing: 4) {
            if ids.isEmpty { Text("Unassigned").font(.caption).foregroundStyle(.secondary) }
            ForEach(ids, id: \.self) { id in
                HStack(spacing: 2) {
                    Text(InputCatalog.displayName(id, controllers: model.input.controllerMonitor.controllers)).font(.caption)
                    Button { model.input.unbind(id, from: action.id) } label: { Image(systemName: "xmark.circle.fill") }
                        .buttonStyle(.borderless).font(.caption2)
                }
                .padding(.horizontal, 6).padding(.vertical, 2)
                .background(Color.secondary.opacity(0.15), in: Capsule())
            }
        }
    }
}

struct TurboTab: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        let c = model.inputConfig
        Form {
            Section("Turbo (Turbo A / B)") {
                Stepper("Period: \(c.turboPeriod) frames", value: Binding(get: { c.turboPeriod }, set: { model.input.setTurbo(period: $0, duty: min(c.turboDuty, $0)) }), in: 2...30)
                Stepper("Press length: \(c.turboDuty) frames", value: Binding(get: { c.turboDuty }, set: { model.input.setTurbo(period: c.turboPeriod, duty: $0) }), in: 1...max(1, c.turboPeriod - 1))
                Text(String(format: String(localized: "About %.1f presses per second. The recording stores the button state after turbo is applied, so playback doesn’t depend on this setting."), 60.0988 / Double(max(1, c.turboPeriod))))
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Simultaneous Opposite Directions (SOCD)") {
                Picker("Policy", selection: Binding(get: { c.socd }, set: { model.input.setSOCD($0) })) {
                    Text("Release both (neutral)").tag("neutral")
                    Text("Last pressed wins").tag("last_wins")
                    Text("Press both (impossible on real hardware)").tag("allow")
                }
            }
            Section("Analog Stick") {
                Slider(value: Binding(get: { c.analogThreshold }, set: { model.input.setAnalogThreshold($0) }), in: 0.2...0.9) {
                    Text("D-pad threshold \(String(format: "%.2f", c.analogThreshold))")
                }
            }
        }
        .formStyle(.grouped)
    }
}

struct DisplayTab: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        Form {
            Section("Display") {
                Picker("Display Size", selection: $model.integerScale) {
                    Text("Pixel-perfect (largest integer scale that fits)").tag(true)
                    Text("FILL (fill the window, aspect ratio kept)").tag(false)
                }
                Toggle("8:7 pixel aspect ratio (as on a CRT TV)", isOn: $model.displayPAR87)
                Toggle("Hide overscan (8 px top and bottom)", isOn: $model.hideOverscan)
                Toggle("Show latency measurement", isOn: $model.showLatency)
            }
            Section("Flash Reduction (Photosensitivity)") {
                Picker("Flash Reduction", selection: $model.flashReduction) {
                    ForEach(FlashLevel.allCases) { Text($0.label).tag($0.rawValue) }
                }
                .pickerStyle(.segmented)
                Text(model.flashLevel.detail).font(.caption)
                Text("Detects scenes where the whole screen flashes hard (explosions, lightning, …) and holds only the display toward the darker side to reduce the number of flashes (based on the WCAG 2.x general flash and red flash thresholds). Small flashes and normal scrolling are shown as they are. Recorded input, game progress and reproducibility are not affected. It can also be applied to MP4 exports.")
                    .font(.caption).foregroundStyle(.secondary)
                Text("Caution: this does not reliably prevent photosensitive seizures. If you feel unwell, stop playing immediately.")
                    .font(.caption).foregroundStyle(.orange)
                Toggle("Show “Flash Reduction Active” on screen while reducing", isOn: $model.showFlashIndicator)
            }
            CRTSettingsSection()
            StreamOutputSection()
            Section("Audio") {
                Slider(value: $model.volume, in: 0...1) { Text("Volume") }
                Text("Audio is muted while paused, rewinding, seeking, in slow motion or fast-forwarding. Sound keeps playing when ReplayNES isn’t in front.").font(.caption).foregroundStyle(.secondary)
            }
            Section("Controls") {
                Toggle("Pause when rewind / fast-forward is released", isOn: $model.pauseAfterRewind)
                Picker("Autosave interval", selection: $model.autosaveInterval) {
                    Text("2 s").tag(2.0)
                    Text("3 s").tag(3.0)
                    Text("5 s").tag(5.0)
                    Text("10 s").tag(10.0)
                    Text("30 s").tag(30.0)
                }
            }
        }
        .formStyle(.grouped)
    }
}
