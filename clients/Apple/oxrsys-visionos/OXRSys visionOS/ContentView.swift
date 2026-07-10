// SPDX-License-Identifier: MPL-2.0

import SwiftUI

struct ContentView: View {
    @Environment(AppModel.self) private var appModel
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        @Bindable var appModel = appModel

        VStack(alignment: .leading, spacing: 14) {
            VStack(alignment: .leading, spacing: 4) {
                Text("OXRSys")
                    .font(.title2)
                    .fontWeight(.semibold)

                Text(appModel.statusText)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
                    .fixedSize(horizontal: false, vertical: true)
            }

            controls

            Divider()

            Toggle("Auto-enter immersive", isOn: $appModel.autoEnterImmersiveOnConnect)
                .disabled(appModel.connectionState == .streaming)

            Toggle("Show hands", isOn: $appModel.showHandsInImmersive)

            Toggle("Keep window in immersive", isOn: $appModel.keepControlWindowVisibleInImmersive)
                .disabled(appModel.connectionState == .streaming)

            DisclosureGroup("Developer") {
                VStack(alignment: .leading, spacing: 10) {
                    Toggle("Emulate controllers (hands + gamepad)", isOn: $appModel.emulateControllers)
                }
                .padding(.top, 4)
            }
        }
        .padding(20)
        .frame(width: 320)
        .task {
            await synchronizePresentationState()
        }
        .onChange(of: appModel.connectionState) { _, _ in
            Task {
                await synchronizePresentationState()
            }
        }
        .onChange(of: appModel.wantsImmersiveSpace) { _, _ in
            Task {
                await synchronizePresentationState()
            }
        }
    }

    @ViewBuilder
    private var controls: some View {
        switch appModel.connectionState {
        case .disconnected:
            if appModel.discoveredServer != nil {
                HStack(spacing: 10) {
                    Button("Connect") {
                        appModel.connect()
                        Task {
                            await synchronizePresentationState()
                        }
                    }
                    .buttonStyle(.borderedProminent)

                    Button("Search Again") {
                        appModel.startDiscovery()
                    }
                }
            } else {
                Button("Find Server") {
                    appModel.startDiscovery()
                }
                .buttonStyle(.borderedProminent)
            }

        case .discovering:
            HStack(spacing: 10) {
                ProgressView()
                    .controlSize(.small)

                Button("Cancel", role: .cancel) {
                    Task {
                        await disconnectAndDismissImmersive()
                    }
                }
            }

        case .connecting:
            HStack(spacing: 10) {
                ProgressView()
                    .controlSize(.small)

                Button("Cancel", role: .cancel) {
                    Task {
                        await disconnectAndDismissImmersive()
                    }
                }
            }

        case .streaming:
            HStack(spacing: 10) {
                Button("Enter Immersive") {
                    appModel.enterImmersiveSpace()
                }
                .buttonStyle(.borderedProminent)
                .disabled(appModel.immersiveSpaceState != .closed)

                Button("Disconnect", role: .destructive) {
                    Task {
                        await disconnectAndDismissImmersive()
                    }
                }
            }
        }
    }

    private func disconnectAndDismissImmersive() async {
        appModel.wantsImmersiveSpace = false
        if appModel.immersiveSpaceState != .closed {
            appModel.immersiveSpaceState = .inTransition
            await dismissImmersiveSpace()
            appModel.immersiveSpaceDidClose()
        }
        appModel.disconnect()
    }

    /// Drives the immersive space from the user's intent (`wantsImmersiveSpace`) rather than the
    /// raw connection state, so exiting via the Digital Crown returns to this menu instead of
    /// auto re-entering. The control window is intentionally left open: `.full` immersion hides
    /// it while immersed, and keeping it alive makes it reappear automatically on exit.
    private func synchronizePresentationState() async {
        let shouldBeImmersed = appModel.connectionState == .streaming && appModel.wantsImmersiveSpace

        if shouldBeImmersed {
            guard appModel.immersiveSpaceState == .closed else { return }
            appModel.immersiveSpaceState = .inTransition
            switch await openImmersiveSpace(id: appModel.immersiveSpaceID) {
            case .opened:
                appModel.immersiveSpaceDidOpen()
                appModel.requestKeyframe()
                hideControlWindowIfNeeded()
            case .userCancelled, .error:
                appModel.immersiveSpaceState = .closed
            @unknown default:
                appModel.immersiveSpaceState = .closed
            }
        } else {
            guard appModel.immersiveSpaceState != .closed else { return }
            appModel.immersiveSpaceState = .inTransition
            await dismissImmersiveSpace()
            appModel.immersiveSpaceDidClose()
            restoreControlWindowIfNeeded()
        }
    }

    private func hideControlWindowIfNeeded() {
        guard !appModel.keepControlWindowVisibleInImmersive else { return }
        appModel.shouldRestoreControlWindowOnImmersiveClose = true
        startControlWindowRestoreWatcher()
        dismissWindow(id: appModel.controlWindowID)
    }

    private func restoreControlWindowIfNeeded() {
        guard appModel.shouldRestoreControlWindowOnImmersiveClose else { return }
        appModel.shouldRestoreControlWindowOnImmersiveClose = false
        openWindow(id: appModel.controlWindowID)
    }

    private func startControlWindowRestoreWatcher() {
        let appModel = appModel
        let openWindow = openWindow

        Task { @MainActor in
            while appModel.shouldRestoreControlWindowOnImmersiveClose {
                if appModel.immersiveSpaceState == .closed {
                    appModel.shouldRestoreControlWindowOnImmersiveClose = false
                    openWindow(id: appModel.controlWindowID)
                    return
                }

                try? await Task.sleep(for: .milliseconds(150))
            }
        }
    }
}

#Preview(windowStyle: .automatic) {
    ContentView()
        .environment(AppModel())
}
