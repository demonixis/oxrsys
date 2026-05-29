// SPDX-License-Identifier: MPL-2.0

import SwiftUI

struct ContentView: View {
    @Environment(AppModel.self) private var appModel
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("OXRSys visionOS")
                .font(.largeTitle)
                .fontWeight(.semibold)

            Text(appModel.statusText)
                .foregroundStyle(.secondary)

            Button("Search") {
                appModel.startDiscovery()
            }
            .buttonStyle(.borderedProminent)
            .disabled(appModel.connectionState != .disconnected)
        }
        .padding(28)
        .frame(width: 360, height: 180)
        .task {
            await synchronizePresentationState()
        }
        .onChange(of: appModel.connectionState) { _, _ in
            Task {
                await synchronizePresentationState()
            }
        }
    }

    private func synchronizePresentationState() async {
        switch appModel.connectionState {
        case .streaming:
            guard appModel.immersiveSpaceState == .closed else { return }
            appModel.immersiveSpaceState = .inTransition
            switch await openImmersiveSpace(id: appModel.immersiveSpaceID) {
            case .opened:
                appModel.immersiveSpaceDidOpen()
                dismissWindow(id: appModel.controlWindowID)
            case .userCancelled, .error:
                appModel.immersiveSpaceState = .closed
            @unknown default:
                appModel.immersiveSpaceState = .closed
            }

        case .disconnected, .discovering:
            guard appModel.immersiveSpaceState == .open else { return }
            appModel.immersiveSpaceState = .inTransition
            await dismissImmersiveSpace()

        case .connecting:
            break
        }
    }
}

#Preview(windowStyle: .automatic) {
    ContentView()
        .environment(AppModel())
}
