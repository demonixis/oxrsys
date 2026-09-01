// SPDX-License-Identifier: MPL-2.0

#if os(iOS)
import MetalKit
import OXRSysStreaming
import SwiftUI
import UIKit

struct MetalView: UIViewRepresentable {
    let renderer: StereoRenderer?
    let refreshRate: Int

    func makeUIView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: renderer?.device)
        view.delegate = renderer
        view.colorPixelFormat = .bgra8Unorm
        view.framebufferOnly = true
        view.preferredFramesPerSecond = refreshRate
        view.isPaused = false
        view.enableSetNeedsDisplay = false
        view.clearColor = MTLClearColor(red: 0.05, green: 0.05, blue: 0.05, alpha: 1)
        return view
    }

    func updateUIView(_ uiView: MTKView, context: Context) {
        uiView.delegate = renderer
        uiView.preferredFramesPerSecond = refreshRate
    }
}
#endif
