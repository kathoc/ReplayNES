// iPhone app entry: the library until a game runs, then the play screen; alerts, prompts and
// sheets; scene phase -> persistence (iOS may end the app any time it is in the background).
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

@main
struct ReplayNESApp: App {
    @StateObject private var model = AppModel.shared
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @Environment(\.scenePhase) private var phase

    init() {
        // The product's core, content identity and ROM filter, before AppModel touches the library.
        RNProduct.startup()
        UILanguage.applyAtLaunch()
        TestHooks.prepare()
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(model)
                .onAppear {
                    model.start()
                    TestHooks.run(model)
                }
                // Opened files come in through SceneDelegate (all URLs of a multi-item open).
        }
        .onChange(of: phase) { _, p in
            switch p {
            case .background: model.enterBackground()
            case .active: model.enterForeground()
            default: break
            }
        }
    }
}

struct RootView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        GeometryReader { geo in
            // A FlipPad over the lower half of the phone held upright: the whole app in the upper half
            // (the safe area's top included), black below (2026-11-21, user request).
            let fullHeight = geo.size.height + geo.safeAreaInsets.top + geo.safeAreaInsets.bottom
            let upper = model.upperHalfController && geo.size.height > geo.size.width
            app(upper: upper)
                .frame(height: upper ? max(200, fullHeight / 2 - geo.safeAreaInsets.top) : geo.size.height)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                .onAppear { model.upperHalfLayout = upper }
                .onChange(of: upper) { _, u in model.upperHalfLayout = u }
        }
        .background(Color.black.ignoresSafeArea())
        .alert(model.alert?.title ?? "", isPresented: Binding(get: { model.alert != nil && !model.upperHalfLayout }, set: { if !$0 { model.alert = nil } }),
               presenting: model.alert) { a in
            ForEach(Array(a.actions.enumerated()), id: \.offset) { _, act in
                Button(act.title, role: act.role) { act.run() }
            }
        } message: { a in Text(a.message) }
        .alert(model.prompt?.title ?? "", isPresented: Binding(get: { model.prompt != nil }, set: { if !$0 { model.prompt = nil } }),
               presenting: model.prompt) { p in
            PromptField(prompt: p)
        }
        .sheet(isPresented: $model.showUnlocks) { UnlocksView().environmentObject(model) }
        .sheet(isPresented: $model.showExport) { ExportView().environmentObject(model) }
    }

    private func app(upper: Bool) -> some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if model.status.hasSession && !model.showLibrary {
                PlayView()
            } else {
                LibraryView()
                    .overlay(alignment: .bottom) { LibraryNotice() }
            }
            if model.showBusy {
                // A session change is saving / loading off the main thread (AppModel.beginBusy).
                ZStack {
                    Color.black.opacity(0.35).ignoresSafeArea()
                    ProgressView().controlSize(.large).tint(.white)
                }
                .contentShape(Rectangle())
                .accessibilityIdentifier("busy")
                .transition(.opacity)
            }
            if upper, let a = model.alert { TopDialog(alert: a) }
        }
        .animation(.easeOut(duration: 0.15), value: model.showBusy)
    }
}

/// The dialog in the upper half (a FlipPad over the lower half): the pad's left / right choose, confirm runs,
/// back cancels (AppModel.handleDialogNav); a tap runs a button.
private struct TopDialog: View {
    @EnvironmentObject var model: AppModel
    let alert: AppAlert

    var body: some View {
        ZStack {
            Color.black.opacity(0.5).ignoresSafeArea()
            VStack(spacing: 10) {
                Text(alert.title).font(.system(size: 17, weight: .semibold)).multilineTextAlignment(.center)
                if !alert.message.isEmpty {
                    Text(alert.message).font(.system(size: 13)).foregroundStyle(.white.opacity(0.75)).multilineTextAlignment(.center)
                        .fixedSize(horizontal: false, vertical: true)
                }
                HStack(spacing: 8) {
                    ForEach(Array(alert.actions.enumerated()), id: \.offset) { i, act in
                        let focused = i == model.dialogFocus
                        Button { model.runDialogAction(i) } label: {
                            Text(act.title).font(.system(size: 15, weight: act.role == .cancel ? .regular : .semibold))
                                .foregroundStyle(act.role == .destructive ? Color(red: 1, green: 0.42, blue: 0.42) : .white)
                                .lineLimit(1).minimumScaleFactor(0.7)
                                .frame(maxWidth: .infinity, minHeight: 40)
                                .background(RoundedRectangle(cornerRadius: 10).fill(Color.white.opacity(focused ? 0.2 : 0.08)))
                                .overlay(RoundedRectangle(cornerRadius: 10).stroke(focused ? Color.white.opacity(0.8) : .clear, lineWidth: 2))
                        }
                        .accessibilityIdentifier("topDialog.\(i)")
                    }
                }
            }
            .padding(16)
            .background(RoundedRectangle(cornerRadius: 16).fill(Color(red: 0.11, green: 0.11, blue: 0.12)))
            .overlay(RoundedRectangle(cornerRadius: 16).stroke(Color.white.opacity(0.1)))
            .padding(.horizontal, 24)
            .foregroundStyle(.white)
            .accessibilityIdentifier("topDialog")
        }
    }
}

/// Notices over the library (the play screen shows its own over the picture): import results.
private struct LibraryNotice: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack {
            if let n = model.notice {
                Text(n).font(.system(size: 14, weight: .medium)).multilineTextAlignment(.center)
                    .padding(.horizontal, 14).padding(.vertical, 9)
                    .background(.black.opacity(0.8), in: RoundedRectangle(cornerRadius: 12))
                    .overlay(RoundedRectangle(cornerRadius: 12).stroke(.white.opacity(0.15)))
                    .foregroundStyle(.white)
                    .accessibilityIdentifier("notice")
                    .transition(.opacity.combined(with: .move(edge: .bottom)))
            }
        }
        .padding(.horizontal, 24)
        .padding(.bottom, 32)
        .allowsHitTesting(false)
        .animation(.easeOut(duration: 0.2), value: model.notice)
    }
}

private struct PromptField: View {
    let prompt: AppPrompt
    @State private var text: String
    init(prompt: AppPrompt) {
        self.prompt = prompt
        _text = State(initialValue: prompt.text)
    }
    var body: some View {
        TextField("", text: $text)
        Button(String(localized: "Rename")) { prompt.done(text) }
        Button(String(localized: "Cancel"), role: .cancel) {}
    }
}
