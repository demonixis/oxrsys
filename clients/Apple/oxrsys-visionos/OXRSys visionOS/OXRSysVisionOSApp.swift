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
        .immersionStyle(selection: .constant(.full), in: .full)
    }
}
