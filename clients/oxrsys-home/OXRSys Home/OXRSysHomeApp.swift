// SPDX-License-Identifier: MPL-2.0

//
//  OXRSysHomeApp.swift
//  OXRSys Home
//
//  Created by Yannick Comte on 19/03/2026.
//

import SwiftUI
import OXRSysSimulator

@main
struct OXRSysHomeApp: App {
    @StateObject private var model = HomeAppModel()
    @StateObject private var preferences = HomePreferences()

    var body: some Scene {
        WindowGroup {
            ContentView(model: model, preferences: preferences)
        }

        Window("OXRSys Simulator", id: HomeWindowID.simulator) {
            OXRSysSimulatorView()
        }
        .defaultSize(width: 1280, height: 720)
    }
}
