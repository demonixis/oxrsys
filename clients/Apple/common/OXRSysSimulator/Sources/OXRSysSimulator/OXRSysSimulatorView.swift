// SPDX-License-Identifier: MPL-2.0

// OXRSysSimulatorView.swift — Unified viewer UI for simulator and stereo modes.

import MetalKit
import OXRSysStreaming
import SwiftUI
import UniformTypeIdentifiers

public struct OXRSysSimulatorView: View {
    @State private var model = SimulatorModel()
    @State private var showSettings = false

    public init() {}

    public var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            switch model.state {
            case .disconnected:
                disconnectedView
            case .discovering:
                discoveryView
            case .connecting:
                connectingView
            case .streaming:
                streamingView
            }
        }
        #if os(iOS)
        .preferredColorScheme(.dark)
        .persistentSystemOverlays(.hidden)
        #endif
        .sheet(isPresented: $showSettings) {
            SettingsSheet(model: model)
            #if os(iOS)
                .presentationDetents([.medium, .large])
                .presentationDragIndicator(.visible)
            #endif
        }
        .onDisappear {
            #if os(macOS)
            if model.inputManager.mouseCaptured {
                CGAssociateMouseAndMouseCursorPosition(1)
                NSCursor.unhide()
            }
            #endif
            model.disconnect()
        }
    }

    private var disconnectedView: some View {
        VStack(spacing: 24) {
            Image(systemName: "visionpro")
                .font(.system(size: 60))
                .foregroundStyle(.white.opacity(0.85))

            Text("OXRSys Simulator")
                .font(.title)
                .fontWeight(.semibold)
                .foregroundStyle(.white)

            Text("Unified streaming client with Simulator and StereoView modes.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)

            Button {
                model.startDiscovery()
            } label: {
                Label("Search for Server", systemImage: "antenna.radiowaves.left.and.right")
                    .font(.headline)
                    .padding(.horizontal, 28)
                    .padding(.vertical, 12)
            }
            .buttonStyle(.borderedProminent)

            #if os(macOS)
            Button {
                showSettings = true
            } label: {
                Label("Settings", systemImage: "gearshape")
            }
            .buttonStyle(.bordered)
            #endif

            Text(model.controlHint)
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 24)
        }
        .padding()
    }

    private var discoveryView: some View {
        VStack(spacing: 20) {
            ProgressView()
                .scaleEffect(1.25)

            Text(model.statusText)
                .font(.headline)
                .foregroundStyle(.white)

            if model.discoveredServer != nil {
                Button {
                    model.connect()
                } label: {
                    Label("Connect", systemImage: "link")
                        .font(.headline)
                        .padding(.horizontal, 28)
                        .padding(.vertical, 12)
                }
                .buttonStyle(.borderedProminent)
            }

            Button("Cancel") {
                model.disconnect()
            }
            .foregroundStyle(.secondary)

            #if os(macOS)
            Button {
                showSettings = true
            } label: {
                Label("Settings", systemImage: "gearshape")
            }
            .buttonStyle(.bordered)
            #endif
        }
        .padding()
    }

    private var connectingView: some View {
        VStack(spacing: 16) {
            ProgressView()
            Text(model.statusText)
                .font(.headline)
                .foregroundStyle(.white)
        }
    }

    private var streamingView: some View {
        ZStack {
            MetalView(renderer: model.renderer, refreshRate: model.refreshRate)
                #if os(macOS)
                .onKeyDown { model.inputManager.onKeyDown($0) }
                .onKeyUp { model.inputManager.onKeyUp($0) }
                .onMouseMotion { dx, dy in model.inputManager.onMouseMotion(deltaX: dx, deltaY: dy) }
                .onScroll { dy in model.inputManager.onScroll(deltaY: dy) }
                .onRightMouseDown {
                    let captured = !model.inputManager.mouseCaptured
                    model.inputManager.setMouseCaptured(captured)
                    CGAssociateMouseAndMouseCursorPosition(captured ? 0 : 1)
                    if captured {
                        NSCursor.hide()
                    } else {
                        NSCursor.unhide()
                    }
                }
                #endif
                .ignoresSafeArea()

            if model.showsSimulationControls {
                #if os(iOS)
                MobileControlsOverlay(inputManager: model.inputManager)
                #endif
            }

            ViewerHUD(model: model, showSettings: $showSettings)
        }
    }
}

private struct ViewerHUD: View {
    let model: SimulatorModel
    @Binding var showSettings: Bool

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Button {
                    showSettings = true
                } label: {
                    Image(systemName: "gearshape.fill")
                        .font(.title3)
                        .foregroundStyle(.white.opacity(0.85))
                }
                .buttonStyle(.plain)

                Spacer()

                badge(icon: "rectangle.compress.vertical", text: model.modeLabel, color: .blue)
                badge(icon: model.isTracking ? "location.fill" : "location.slash",
                      text: model.isTracking ? "Tracking" : "Tracking Lost",
                      color: model.isTracking ? .green : .red)
                badge(icon: "video.fill",
                      text: "\(model.framesDecoded) frames",
                      color: .cyan)

                Spacer()

                Button {
                    model.disconnect()
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.title3)
                        .foregroundStyle(.white.opacity(0.85))
                }
                .buttonStyle(.plain)
            }
            .padding(.horizontal, 16)
            .padding(.top, 14)

            if model.showStats {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 10) {
                        badge(
                            icon: "arrow.down.circle",
                            text: String(format: "RECV %.0f fps", model.stats.deliveryFps),
                            color: model.stats.deliveryFps > 25 ? .green : .orange
                        )
                        badge(
                            icon: "wifi.slash",
                            text: String(format: "LOSS %.1f%%", model.stats.lossPercent),
                            color: model.stats.lossPercent > 5 ? .red : model.stats.lossPercent > 1 ? .yellow : .green
                        )
                        badge(
                            icon: "exclamationmark.triangle",
                            text: "ERR \(model.stats.decodeErrors)",
                            color: model.stats.decodeErrors > 0 ? .red : .green
                        )
                        badge(
                            icon: "xmark.bin",
                            text: "DROP \(model.stats.framesDropped)",
                            color: model.stats.framesDropped > 0 ? .orange : .green
                        )
                        badge(
                            icon: "shippingbox",
                            text: "PKTS \(model.stats.packetsReceived)",
                            color: .white
                        )
                    }
                    .padding(.horizontal, 16)
                    .padding(.top, 10)
                }
            }

            HStack {
                Text(model.controlHint)
                    .font(.caption)
                    .foregroundStyle(.white.opacity(0.85))
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .background(.black.opacity(0.5), in: Capsule())
                Spacer()
            }
            .padding(.horizontal, 16)
            .padding(.top, 10)

            Spacer()
        }
    }

    private func badge(icon: String, text: String, color: Color) -> some View {
        HStack(spacing: 5) {
            Image(systemName: icon)
                .font(.caption)
            Text(text)
                .font(.caption)
                .monospacedDigit()
        }
        .foregroundStyle(color)
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(.black.opacity(0.55), in: Capsule())
    }
}

private struct SettingsSheet: View {
    @Bindable var model: SimulatorModel
    @Environment(\.dismiss) private var dismiss
    @State private var showCalibrationImporter = false

    var body: some View {
        #if os(macOS)
        macOSSettings
        #else
        NavigationStack {
            Form {
                modeSection
                stereoSection
                controlsSection
            }
            .navigationTitle("Viewer Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Close") {
                        dismiss()
                    }
                }
            }
        }
        #endif
    }

    private var modeSection: some View {
        Section("Mode") {
            Picker("Viewer Mode", selection: $model.viewerMode) {
                ForEach(model.availableViewerModes()) { mode in
                    Text(mode.rawValue).tag(mode)
                }
            }
            #if os(iOS)
            .pickerStyle(.segmented)
            #endif

            Toggle("Show Streaming Stats", isOn: $model.showStats)
        }
    }

    private var stereoSection: some View {
        Section("Stereo") {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("IPD Offset")
                    Spacer()
                    Text(String(format: "%.3f", model.ipdOffset))
                        .foregroundStyle(.secondary)
                        .monospacedDigit()
                }
                Slider(value: $model.ipdOffset, in: -0.05...0.05, step: 0.001)
                HStack {
                    Text("Convergent")
                    Spacer()
                    Text("Divergent")
                }
                .font(.caption2)
                .foregroundStyle(.secondary)
            }

            Button("Reset IPD") {
                model.ipdOffset = 0
            }
            .foregroundStyle(.secondary)
        }
    }

    private var controlsSection: some View {
        Section("Controls") {
            Text(model.controlHint)
                .foregroundStyle(.secondary)

            if model.canResetPose {
                Button {
                    model.resetPose()
                    dismiss()
                } label: {
                    Label("Reset Position and Orientation", systemImage: "arrow.counterclockwise")
                }
            }

            Button {
                model.requestKeyframe()
            } label: {
                Label("Request Keyframe", systemImage: "arrow.clockwise.icloud")
            }
        }
    }

    #if os(macOS)
    private var macOSSettings: some View {
        NavigationStack {
            TabView {
                Form {
                    modeSection
                    stereoSection
                    controlsSection
                }
                .formStyle(.grouped)
                .tabItem {
                    Label("General", systemImage: "slider.horizontal.3")
                }

                ScrollView {
                    VStack(alignment: .leading, spacing: 14) {
                        webcamSourcePanel
                        webcamRigPanel
                        webcamHeadPanel
                        webcamHandPanel
                        webcamStatusPanel
                    }
                    .padding(20)
                    .frame(maxWidth: .infinity, alignment: .topLeading)
                }
                .scrollIndicators(.visible)
                .tabItem {
                    Label("Webcam", systemImage: "camera")
                }
            }
            .navigationTitle("Viewer Settings")
            .fileImporter(
                isPresented: $showCalibrationImporter,
                allowedContentTypes: [.json]
            ) { result in
                if case let .success(url) = result {
                    model.importWebcamCalibration(from: url)
                }
            }
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Close") {
                        dismiss()
                    }
                    .keyboardShortcut(.defaultAction)
                }
            }
        }
        .frame(
            minWidth: 560,
            idealWidth: 680,
            maxWidth: .infinity,
            minHeight: 440,
            idealHeight: 640,
            maxHeight: .infinity
        )
    }

    private var webcamSourcePanel: some View {
        GroupBox("Source") {
            VStack(alignment: .leading, spacing: 12) {
                Toggle("Use Webcam Tracking", isOn: $model.webcamTrackingEnabled)

                Picker("Output", selection: $model.webcamOutputMode) {
                    ForEach(WebcamTrackingOutputMode.allCases) { mode in
                        Text(mode.rawValue).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                .disabled(!model.webcamTrackingEnabled)

                Toggle("Head Tracking", isOn: $model.webcamHeadTrackingEnabled)
                    .disabled(!model.webcamTrackingEnabled)

                Picker("Tracking Space", selection: $model.webcamTrackingSpace) {
                    ForEach(WebcamTrackingSpace.allCases) { space in
                        Text(space.rawValue).tag(space)
                    }
                }
                .disabled(!model.webcamTrackingEnabled)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.vertical, 4)
        }
    }

    private var webcamRigPanel: some View {
        GroupBox("Camera Rig") {
            VStack(alignment: .leading, spacing: 12) {
                Picker("Mode", selection: $model.webcamSourceMode) {
                    ForEach(WebcamTrackingSourceMode.allCases) { mode in
                        Text(mode.rawValue).tag(mode)
                    }
                }

                Picker("Capture", selection: $model.webcamCaptureResolution) {
                    ForEach(WebcamCaptureResolution.allCases) { resolution in
                        Text(resolution.rawValue).tag(resolution)
                    }
                }

                Picker("Camera Facing", selection: $model.webcamCameraFacing) {
                    ForEach(WebcamCameraFacing.allCases) { facing in
                        Text(facing.rawValue).tag(facing)
                    }
                }

                Picker("Primary Camera", selection: $model.selectedWebcamID) {
                    ForEach(model.webcamDeviceChoices) { device in
                        Text(device.name).tag(device.id)
                    }
                }

                Picker("Secondary Camera", selection: $model.secondaryWebcamID) {
                    ForEach(model.webcamSecondaryDeviceChoices) { device in
                        Text(device.name).tag(device.id)
                    }
                }
                .disabled(model.webcamSourceMode == .singleCamera)

                HStack {
                    if model.webcamPreviewWindowsOpen {
                        Button {
                            model.closeWebcamPreviewWindows()
                        } label: {
                            Label("Close Previews", systemImage: "eye.slash")
                        }
                    } else {
                        Button {
                            model.openWebcamPreviewWindows()
                        } label: {
                            Label("Open Previews", systemImage: "eye")
                        }
                        .disabled(!model.webcamTrackingEnabled)
                    }

                    Button {
                        showCalibrationImporter = true
                    } label: {
                        Label("Import Calibration", systemImage: "square.and.arrow.down")
                    }

                    Button {
                        model.resetWebcamCalibration()
                    } label: {
                        Label("Reset", systemImage: "xmark.circle")
                    }
                    .foregroundStyle(.secondary)
                }

                Text(model.webcamCalibrationStatusText)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.vertical, 4)
        }
    }

    private var webcamHeadPanel: some View {
        GroupBox("Head Calibration") {
            VStack(alignment: .leading, spacing: 14) {
                metricSlider(
                    title: "Camera Y",
                    value: $model.webcamCameraY,
                    range: 0.2...2.4,
                    step: 0.01,
                    unit: "m"
                )

                Text("Head Offset")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                axisSliderGrid(
                    x: $model.webcamHeadOffsetX,
                    y: $model.webcamHeadOffsetY,
                    z: $model.webcamHeadOffsetZ,
                    range: -1...1,
                    step: 0.01,
                    unit: "m"
                )

                metricSlider(
                    title: "Move Deadzone",
                    value: $model.webcamMovementDeadzone,
                    range: 0...0.1,
                    step: 0.001,
                    unit: "m",
                    precision: 3
                )

                Text("Head Position Interpolation")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                axisSliderGrid(
                    x: $model.webcamHeadPositionInterpolationX,
                    y: $model.webcamHeadPositionInterpolationY,
                    z: $model.webcamHeadPositionInterpolationZ,
                    range: 0...1,
                    step: 0.01,
                    unit: ""
                )

                Text("Head Rotation Interpolation")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                axisSliderGrid(
                    x: $model.webcamHeadRotationInterpolationYaw,
                    y: $model.webcamHeadRotationInterpolationPitch,
                    z: $model.webcamHeadRotationInterpolationRoll,
                    range: 0...1,
                    step: 0.01,
                    unit: "",
                    labels: ("Yaw", "Pitch", "Roll")
                )

                metricSlider(
                    title: "Rot Deadzone",
                    value: $model.webcamHeadRotationDeadzoneDegrees,
                    range: 0...10,
                    step: 0.1,
                    unit: "deg",
                    precision: 1
                )

                Text("Head Rotation Limits")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                axisSliderGrid(
                    x: $model.webcamHeadRotationLimitYawDegrees,
                    y: $model.webcamHeadRotationLimitPitchDegrees,
                    z: $model.webcamHeadRotationLimitRollDegrees,
                    range: 0...90,
                    step: 1,
                    unit: "deg",
                    labels: ("Yaw", "Pitch", "Roll"),
                    precision: 0
                )
            }
            .disabled(!model.webcamTrackingEnabled)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.vertical, 4)
        }
    }

    private var webcamHandPanel: some View {
        GroupBox("Hand and Controller Calibration") {
            VStack(alignment: .leading, spacing: 14) {
                metricSlider(
                    title: "Hands Y Offset",
                    value: $model.webcamHandYOffset,
                    range: -1...1,
                    step: 0.01,
                    unit: "m"
                )

                Text("Hands Position Interpolation")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                axisSliderGrid(
                    x: $model.webcamHandPositionInterpolationX,
                    y: $model.webcamHandPositionInterpolationY,
                    z: $model.webcamHandPositionInterpolationZ,
                    range: 0...1,
                    step: 0.01,
                    unit: ""
                )

                metricSlider(
                    title: "Rotation Interp",
                    value: $model.webcamHandRotationInterpolation,
                    range: 0...1,
                    step: 0.01,
                    unit: ""
                )

                metricSlider(
                    title: "Depth Scale",
                    value: $model.webcamHandDepthScale,
                    range: 0.5...1.8,
                    step: 0.01,
                    unit: ""
                )

                metricSlider(
                    title: "Depth Offset",
                    value: $model.webcamHandDepthOffset,
                    range: -0.5...0.5,
                    step: 0.01,
                    unit: "m"
                )

                metricSlider(
                    title: "Depth Smoothing",
                    value: $model.webcamHandDepthSmoothing,
                    range: 0...1,
                    step: 0.01,
                    unit: ""
                )

                Text("Controller Rotation Offset")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                axisSliderGrid(
                    x: $model.webcamControllerRotationXDegrees,
                    y: $model.webcamControllerRotationYDegrees,
                    z: $model.webcamControllerRotationZDegrees,
                    range: 0...360,
                    step: 1,
                    unit: "deg",
                    labels: ("X", "Y", "Z"),
                    precision: 0
                )
            }
            .disabled(!model.webcamTrackingEnabled)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.vertical, 4)
        }
    }

    private var webcamStatusPanel: some View {
        HStack {
            Text(model.webcamStatusText)
                .foregroundStyle(.secondary)
            Spacer()
            Button("Refresh Cameras") {
                model.refreshWebcamDevices()
            }
        }
    }

    private func axisSliderGrid(
        x: Binding<Float>,
        y: Binding<Float>,
        z: Binding<Float>,
        range: ClosedRange<Float>,
        step: Float,
        unit: String,
        labels: (String, String, String) = ("X", "Y", "Z"),
        precision: Int = 2
    ) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            metricSlider(title: labels.0, value: x, range: range, step: step, unit: unit, precision: precision)
            metricSlider(title: labels.1, value: y, range: range, step: step, unit: unit, precision: precision)
            metricSlider(title: labels.2, value: z, range: range, step: step, unit: unit, precision: precision)
        }
    }

    private func metricSlider(
        title: String,
        value: Binding<Float>,
        range: ClosedRange<Float>,
        step: Float,
        unit: String,
        precision: Int = 2
    ) -> some View {
        HStack(spacing: 12) {
            Text(title)
                .frame(width: 110, alignment: .leading)
            Slider(value: value, in: range, step: step)
                .frame(minWidth: 180)
            Text(formattedMetric(value.wrappedValue, unit: unit, precision: precision))
                .foregroundStyle(.secondary)
                .monospacedDigit()
                .frame(width: 72, alignment: .trailing)
        }
    }

    private func formattedMetric(_ value: Float, unit: String, precision: Int) -> String {
        let number = String(format: "%.\(precision)f", value)
        if unit.isEmpty {
            return number
        }
        return "\(number) \(unit)"
    }
    #endif
}

#if os(iOS)
private struct MobileControlsOverlay: View {
    let inputManager: SimulatorInputManager

    var body: some View {
        VStack {
            Spacer()
            HStack(alignment: .bottom) {
                JoystickView(label: "Move") { x, y in
                    inputManager.setMoveJoystick(x, y)
                }
                .padding(.leading, 28)

                Spacer()

                JoystickView(label: "Look") { x, y in
                    inputManager.setLookJoystick(x, y)
                }
                .padding(.trailing, 28)
            }
            .padding(.bottom, 42)
        }
    }
}

private struct JoystickView: View {
    let label: String
    let onChange: (Float, Float) -> Void

    @State private var offset: CGSize = .zero
    private let radius: CGFloat = 55

    var body: some View {
        ZStack {
            Circle()
                .fill(.white.opacity(0.12))
                .frame(width: radius * 2, height: radius * 2)
                .overlay(Circle().stroke(.white.opacity(0.25), lineWidth: 1))

            Circle()
                .fill(.white.opacity(0.4))
                .frame(width: radius * 0.75, height: radius * 0.75)
                .offset(offset)

            Text(label)
                .font(.system(.caption2, design: .rounded))
                .foregroundStyle(.white.opacity(0.6))
                .offset(y: radius + 14)
        }
        .contentShape(Rectangle().size(CGSize(width: radius * 2 + 20, height: radius * 2 + 20)))
        .gesture(
            DragGesture(minimumDistance: 0, coordinateSpace: .local)
                .onChanged { drag in
                    let t = drag.translation
                    let len = sqrt(t.width * t.width + t.height * t.height)
                    let clamped: CGSize = len > radius
                        ? CGSize(width: t.width / len * radius, height: t.height / len * radius)
                        : t
                    offset = clamped
                    onChange(Float(clamped.width / radius), Float(clamped.height / radius))
                }
                .onEnded { _ in
                    offset = .zero
                    onChange(0, 0)
                }
        )
    }
}
#endif

#if os(macOS)
private struct MetalView: NSViewRepresentable {
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

private final class SimulatorMTKView: MTKView {
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
#else
private struct MetalView: UIViewRepresentable {
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
