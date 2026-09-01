// SPDX-License-Identifier: MPL-2.0

#if os(macOS)
import AppKit
import MetalKit
import OXRSysStreaming
import SwiftUI

struct MetalView: NSViewRepresentable {
    let renderer: StereoRenderer?
    let refreshRate: Int

    var keyDownHandler: ((UInt16) -> Void)?
    var keyUpHandler: ((UInt16) -> Void)?
    var mouseMotionHandler: ((Float, Float) -> Void)?
    var scrollHandler: ((Float) -> Void)?
    var rightMouseDownHandler: (() -> Void)?

    func makeNSView(context: Context) -> SimulatorMTKView {
        let view = SimulatorMTKView()
        view.device = renderer?.device
        view.delegate = renderer
        view.colorPixelFormat = .bgra8Unorm
        view.preferredFramesPerSecond = refreshRate
        view.isPaused = false
        view.enableSetNeedsDisplay = false
        view.clearColor = MTLClearColor(red: 0.05, green: 0.05, blue: 0.05, alpha: 1)
        view.keyDownHandler = keyDownHandler
        view.keyUpHandler = keyUpHandler
        view.mouseMotionHandler = mouseMotionHandler
        view.scrollHandler = scrollHandler
        view.rightMouseDownHandler = rightMouseDownHandler
        return view
    }

    func updateNSView(_ nsView: SimulatorMTKView, context: Context) {
        nsView.delegate = renderer
        nsView.preferredFramesPerSecond = refreshRate
        nsView.keyDownHandler = keyDownHandler
        nsView.keyUpHandler = keyUpHandler
        nsView.mouseMotionHandler = mouseMotionHandler
        nsView.scrollHandler = scrollHandler
        nsView.rightMouseDownHandler = rightMouseDownHandler
        nsView.ensureFirstResponder()
    }

    func onKeyDown(_ handler: @escaping (UInt16) -> Void) -> MetalView {
        var copy = self
        copy.keyDownHandler = handler
        return copy
    }

    func onKeyUp(_ handler: @escaping (UInt16) -> Void) -> MetalView {
        var copy = self
        copy.keyUpHandler = handler
        return copy
    }

    func onMouseMotion(_ handler: @escaping (Float, Float) -> Void) -> MetalView {
        var copy = self
        copy.mouseMotionHandler = handler
        return copy
    }

    func onScroll(_ handler: @escaping (Float) -> Void) -> MetalView {
        var copy = self
        copy.scrollHandler = handler
        return copy
    }

    func onRightMouseDown(_ handler: @escaping () -> Void) -> MetalView {
        var copy = self
        copy.rightMouseDownHandler = handler
        return copy
    }
}

final class SimulatorMTKView: MTKView {
    var keyDownHandler: ((UInt16) -> Void)?
    var keyUpHandler: ((UInt16) -> Void)?
    var mouseMotionHandler: ((Float, Float) -> Void)?
    var scrollHandler: ((Float) -> Void)?
    var rightMouseDownHandler: (() -> Void)?

    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    func ensureFirstResponder() {
        guard let window else { return }
        if window.firstResponder !== self {
            window.makeFirstResponder(self)
        }
    }

    override func mouseDown(with event: NSEvent) {
        ensureFirstResponder()
        super.mouseDown(with: event)
    }

    override func keyDown(with event: NSEvent) {
        keyDownHandler?(event.keyCode)
    }

    override func keyUp(with event: NSEvent) {
        keyUpHandler?(event.keyCode)
    }

    override func mouseMoved(with event: NSEvent) {
        mouseMotionHandler?(Float(event.deltaX), Float(event.deltaY))
    }

    override func mouseDragged(with event: NSEvent) {
        mouseMotionHandler?(Float(event.deltaX), Float(event.deltaY))
    }

    override func rightMouseDown(with event: NSEvent) {
        ensureFirstResponder()
        rightMouseDownHandler?()
    }

    override func scrollWheel(with event: NSEvent) {
        ensureFirstResponder()
        scrollHandler?(Float(event.deltaY))
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach { removeTrackingArea($0) }
        addTrackingArea(NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
            owner: self,
            userInfo: nil
        ))
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        ensureFirstResponder()
    }

    override func becomeFirstResponder() -> Bool {
        true
    }
}
#endif
