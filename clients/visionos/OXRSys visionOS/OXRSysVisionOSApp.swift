// SPDX-License-Identifier: MPL-2.0

import CompositorServices
import SwiftUI

struct ImmersiveSpaceContent: CompositorContent {
    var appModel: AppModel

    var body: some CompositorContent {
        CompositorLayer(configuration: self) { @MainActor layerRenderer in
            ImmersiveRenderer.startRenderLoop(layerRenderer, appModel: appModel, worldTracking: appModel.sharedWorldTracking)
        }
    }
}

extension ImmersiveSpaceContent: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        let foveationEnabled = capabilities.supportsFoveation
        configuration.isFoveationEnabled = foveationEnabled

        let options: LayerRenderer.Capabilities.SupportedLayoutsOptions = foveationEnabled ? [.foveationEnabled] : []
        let supportedLayouts = capabilities.supportedLayouts(options: options)
        configuration.layout = supportedLayouts.contains(.layered) ? .layered : .dedicated
    }
}

@main
struct Vision_PlayerApp: App {

    @State private var appModel = AppModel()

    var body: some SwiftUI.Scene {
        WindowGroup(id: appModel.controlWindowID) {
            ContentView()
                .environment(appModel)
        }

        ImmersiveSpace(id: appModel.immersiveSpaceID) {
            ImmersiveSpaceContent(appModel: appModel)
        }
        // .mixed by default: the renderer draws the video opaque, so it still looks fully
        // immersive, but mixed immersion lets you walk the whole room (no safe-bubble
        // breakthrough that .full forces). Toggling useFullImmersion transitions live.
        .immersionStyle(
            selection: Binding<any ImmersionStyle>(
                get: { appModel.useFullImmersion ? .full : .mixed },
                set: { appModel.useFullImmersion = $0 is FullImmersionStyle }
            ),
            in: .mixed, .full)
        .upperLimbVisibility(appModel.showHandsInImmersive ? .visible : .hidden)
    }
}
