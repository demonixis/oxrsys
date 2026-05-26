// SPDX-License-Identifier: MPL-2.0

// OXRSysSimulatorApp.swift — App entry point with window configuration.

import OXRSysSimulator
import SwiftUI

@main
struct OXRSysSimulatorApp: App {
    var body: some Scene {
        WindowGroup {
            OXRSysSimulatorView()
        }
        #if os(macOS)
        .defaultSize(width: 1280, height: 720)
        #endif
    }
}
