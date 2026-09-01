// SPDX-License-Identifier: MPL-2.0

// OXRSysSimulatorView.swift — Unified viewer UI for simulator and stereo modes.

import OXRSysStreaming
import SwiftUI

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
                SimulatorPlatform.setPointerCaptured(false)
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
                    SimulatorPlatform.setPointerCaptured(captured)
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

    var body: some View {
        NavigationStack {
            Form {
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

                if model.showsSimulationControls {
                    Section("Simulator") {
                        VStack(alignment: .leading, spacing: 8) {
                            HStack {
                                Text("Vertical FOV")
                                Spacer()
                                Text("\(model.simulatorFovDegrees) degrees")
                                    .foregroundStyle(.secondary)
                                    .monospacedDigit()
                            }
                            Slider(
                                value: Binding(
                                    get: { Double(model.simulatorFovDegrees) },
                                    set: { model.simulatorFovDegrees = Int($0.rounded()) }
                                ),
                                in: 60...150,
                                step: 1
                            )
                        }
                    }
                }

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
            .navigationTitle("Viewer Settings")
            #if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
            #endif
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Close") {
                        dismiss()
                    }
                }
            }
        }
        #if os(macOS)
        .frame(minWidth: 420, minHeight: 360)
        #endif
    }
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
