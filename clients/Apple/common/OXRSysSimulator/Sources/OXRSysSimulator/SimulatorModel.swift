// SPDX-License-Identifier: MPL-2.0

// SimulatorModel.swift — Unified cross-platform viewer model.

import CoreMedia
import CoreVideo
import Foundation
import Observation
import simd
import SwiftUI
import OXRSysStreaming

@Observable
final class SimulatorModel {
    enum State {
        case disconnected
        case discovering
        case connecting
        case streaming
    }

    enum ViewerMode: String, CaseIterable, Identifiable {
        case simulator = "Simulator"
        case stereoView = "StereoView"

        var id: String { rawValue }

        var rendererMode: StereoDisplayMode {
            switch self {
            case .simulator:
                return .monoPreview
            case .stereoView:
                return .stereoView
            }
        }
    }

    struct StreamStats {
        var packetsReceived: UInt32 = 0
        var framesDelivered: UInt32 = 0
        var framesDropped: UInt32 = 0
        var totalFramesSeen: UInt32 = 0
        var decodeErrors: Int = 0
        var lossPercent: Double = 0
        var deliveryFps: Double = 0
    }

    var state: State = .disconnected
    var discoveredServer: DiscoveredServer?
    var statusText: String
    var framesDecoded: UInt64 = 0
    var isTracking: Bool = false
    var stats = StreamStats()
    var showStats: Bool = true
    var viewerMode: ViewerMode {
        didSet {
            renderer?.displayMode = viewerMode.rendererMode
            if state == .streaming {
                reconfigureTracking()
            }
        }
    }
    var ipdOffset: Float = 0 {
        didSet { renderer?.ipdOffset = ipdOffset }
    }

    private var statsTimer: Timer?
    private var lastStatsTime: Int64 = 0
    private var lastStatsFramesDelivered: UInt32 = 0

    let renderer: StereoRenderer?
    private let discovery = DiscoveryClient()
    private let videoReceiver = VideoReceiver()
    private let trackingSender = TrackingSender()
    private let controlChannel = ControlChannel()
    private let decoder = H265Decoder()
    private let latencyReporter = LatencyReporter()
    let inputManager = SimulatorInputManager()

    private var trackingSource: DispatchSourceTimer?
    private var lastTrackingTimeNs: Int64 = 0

    private var consecutiveDecodeErrors: Int = 0
    private var lastKeyframeRequestTime: UInt64 = 0
    private var lastObservedDroppedFrames: UInt32 = 0
    private let keyframeErrorThreshold = 3
    private let keyframeRequestCooldownNs: UInt64 = 1_000_000_000

    #if os(iOS)
    private let arTracking = ARTrackingManager()
    #endif
    #if os(macOS)
    private let desktopWebcamTracking = DesktopWebcamTrackingManager()
    private let webcamPreviewWindows = WebcamPreviewWindowManager()
    private let webcamSnapshotLock = NSLock()
    private var latestWebcamSnapshot: WebcamTrackingSnapshot?
    private var webcamPreviewGeneration: UInt64 = 0
    var webcamTrackingEnabled: Bool = false {
        didSet {
            guard oldValue != webcamTrackingEnabled else { return }
            if webcamTrackingEnabled {
                startDesktopWebcamTrackingIfNeeded()
            } else {
                closeWebcamPreviewWindows()
                desktopWebcamTracking.stop()
                storeWebcamSnapshot(nil)
                webcamStatusText = "Webcam tracking off"
            }
        }
    }
    var webcamHeadTrackingEnabled: Bool = false {
        didSet {
            guard webcamHeadTrackingEnabled, oldValue != webcamHeadTrackingEnabled else { return }
            desktopWebcamTracking.resetHeadPose()
        }
    }
    var webcamOutputMode: WebcamTrackingOutputMode = .hands
    var webcamSourceMode: WebcamTrackingSourceMode = .singleCamera {
        didSet {
            guard oldValue != webcamSourceMode else { return }
            restartDesktopWebcamTrackingIfNeeded()
        }
    }
    var webcamCaptureResolution: WebcamCaptureResolution = .medium {
        didSet {
            guard oldValue != webcamCaptureResolution else { return }
            restartDesktopWebcamTrackingIfNeeded()
        }
    }
    var webcamTrackingSpace: WebcamTrackingSpace = .localFloor {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamCameraFacing: WebcamCameraFacing = .userFacing {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamCameraY: Float = 1.6 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadOffsetX: Float = 0 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadOffsetY: Float = 0 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadOffsetZ: Float = 0 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHandYOffset: Float = 0.5 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadPositionInterpolationX: Float = 0.01 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadPositionInterpolationY: Float = 0.01 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadPositionInterpolationZ: Float = 0.01 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadRotationInterpolationYaw: Float = 0.03 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadRotationInterpolationPitch: Float = 0.03 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadRotationInterpolationRoll: Float = 0.03 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHandPositionInterpolationX: Float = 0.25 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHandPositionInterpolationY: Float = 0.25 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHandPositionInterpolationZ: Float = 0.25 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHandRotationInterpolation: Float = 0.25 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHandDepthScale: Float = 1.0 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHandDepthOffset: Float = 0.0 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHandDepthSmoothing: Float = 0.35 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamMovementDeadzone: Float = 0.015 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadRotationDeadzoneDegrees: Float = 1.5 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadRotationLimitYawDegrees: Float = 45 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadRotationLimitPitchDegrees: Float = 35 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamHeadRotationLimitRollDegrees: Float = 8 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamControllerRotationXDegrees: Float = 90 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamControllerRotationYDegrees: Float = 0 {
        didSet { updateDesktopWebcamSettings() }
    }
    var webcamControllerRotationZDegrees: Float = 0 {
        didSet { updateDesktopWebcamSettings() }
    }
    var selectedWebcamID: String = "" {
        didSet {
            guard oldValue != selectedWebcamID else { return }
            if secondaryWebcamID == selectedWebcamID {
                secondaryWebcamID = ""
            }
            restartDesktopWebcamTrackingIfNeeded()
        }
    }
    var secondaryWebcamID: String = "" {
        didSet {
            guard oldValue != secondaryWebcamID else { return }
            if secondaryWebcamID == selectedWebcamID {
                secondaryWebcamID = ""
                return
            }
            restartDesktopWebcamTrackingIfNeeded()
        }
    }
    var webcamDevices: [DesktopWebcamDevice] = []
    var webcamStatusText: String = "Webcam tracking off"
    var webcamCalibrationStatusText: String = "No calibration loaded"
    var webcamPreviewWindowsOpen: Bool = false
    private var webcamRigCalibration: WebcamRigCalibration?

    var webcamDeviceChoices: [DesktopWebcamDevice] {
        [DesktopWebcamDevice(id: "", name: "Default Camera")] + webcamDevices
    }

    var webcamSecondaryDeviceChoices: [DesktopWebcamDevice] {
        [DesktopWebcamDevice(id: "", name: "Auto Secondary Camera")] +
            webcamDevices.filter { $0.id != selectedWebcamID }
    }
    #endif

    init() {
        let initialViewerMode: ViewerMode
        let initialStatusText: String
        #if os(iOS)
        initialViewerMode = .stereoView
        initialStatusText = "Tap Search to find the runtime"
        #else
        initialViewerMode = .simulator
        initialStatusText = "Click Search to find the runtime"
        #endif
        viewerMode = initialViewerMode
        statusText = initialStatusText

        guard let device = MTLCreateSystemDefaultDevice() else {
            renderer = nil
            statusText = "Metal not available"
            return
        }

        let renderer = StereoRenderer(device: device)
        renderer?.displayMode = initialViewerMode.rendererMode
        renderer?.ipdOffset = 0
        self.renderer = renderer

        #if os(macOS)
        refreshWebcamDevices()
        updateDesktopWebcamSettings()
        desktopWebcamTracking.onTrackingUpdate = { [weak self] snapshot in
            self?.storeWebcamSnapshot(snapshot)
            Task { @MainActor [weak self] in
                guard let self, self.webcamTrackingEnabled else { return }
                self.isTracking = snapshot.isTracking
            }
        }
        desktopWebcamTracking.onStatusUpdate = { [weak self] status in
            Task { @MainActor [weak self] in
                self?.webcamStatusText = status
            }
        }
        desktopWebcamTracking.onPreviewUpdate = { [weak self] frame in
            Task { @MainActor [weak self] in
                self?.handleWebcamPreviewFrame(frame)
            }
        }
        webcamPreviewWindows.onAllWindowsClosed = { [weak self] in
            guard let self else { return }
            self.webcamPreviewGeneration &+= 1
            self.webcamPreviewWindowsOpen = false
            self.desktopWebcamTracking.setPreviewEnabled(false, generation: self.webcamPreviewGeneration)
        }
        #endif
    }

    var refreshRate: Int {
        Int(discoveredServer?.refreshRate ?? 60)
    }

    var modeLabel: String {
        viewerMode.rawValue
    }

    var showsSimulationControls: Bool {
        viewerMode == .simulator
    }

    var canResetPose: Bool {
        #if os(macOS)
        return webcamTrackingEnabled && webcamHeadTrackingEnabled
        #else
        #if os(iOS)
        return viewerMode == .stereoView
        #else
        return false
        #endif
        #endif
    }

    var controlHint: String {
        #if os(macOS)
        if webcamTrackingEnabled {
            return "Webcam tracking active; keyboard and mouse remain head-pose fallback"
        }
        return "ZQSD/WASD move, mouse look, right-click captures the cursor"
        #else
        switch viewerMode {
        case .simulator:
            return "Use the touch joysticks to move and look around"
        case .stereoView:
            return "Move the device to drive the stereo headset pose"
        }
        #endif
    }

    func availableViewerModes() -> [ViewerMode] {
        #if os(iOS)
        return ViewerMode.allCases
        #else
        return [.simulator]
        #endif
    }

    func startDiscovery() {
        guard state == .disconnected else { return }
        state = .discovering
        statusText = "Searching for server..."

        discovery.start { [weak self] server in
            Task { @MainActor [weak self] in
                guard let self, self.state == .discovering else { return }
                self.discoveredServer = server
                self.statusText = "Found: \(server.name) (\(server.resolution) @ \(server.refreshRate)Hz)"
            }
        }
    }

    func connect() {
        guard let server = discoveredServer else { return }
        state = .connecting
        statusText = "Connecting to \(server.name)..."

        decoder.configure { [weak self] pixelBuffer, presentationTime in
            guard let self else { return }
            self.consecutiveDecodeErrors = 0
            self.renderer?.submitFrame(pixelBuffer)
            self.latencyReporter.noteFrameDecoded(
                presentationTimeNs: Self.nanoseconds(from: presentationTime),
                decodeTimeNs: VideoReceiver.monotonicNs(),
                refreshRateHz: self.refreshRate,
                controlChannel: self.controlChannel
            )
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.framesDecoded += 1
            }
        }

        decoder.onDecodeError = { [weak self] in
            guard let self else { return }
            self.consecutiveDecodeErrors += 1
            if self.consecutiveDecodeErrors >= self.keyframeErrorThreshold {
                let now = VideoReceiver.monotonicNs()
                let elapsed = UInt64(now) - self.lastKeyframeRequestTime
                if elapsed > self.keyframeRequestCooldownNs {
                    self.lastKeyframeRequestTime = UInt64(now)
                    self.consecutiveDecodeErrors = 0
                    self.controlChannel.requestKeyframe(
                        reason: KeyframeReason.decodeStall.rawValue
                    )
                    print("[Viewer] Requested keyframe after decode errors")
                }
            }
        }

        videoReceiver.start { [weak self] nalData, presentationTimeNs, receiveTimeNs in
            guard let self else { return }
            self.latencyReporter.noteFrameReceived(
                presentationTimeNs: presentationTimeNs,
                receiveTimeNs: receiveTimeNs
            )
            self.decoder.decode(nalData: nalData, presentationTimeNs: presentationTimeNs)
        }

        Thread.sleep(forTimeInterval: 0.05)

        discovery.sendConnect(
            to: server,
            deviceName: "OXRSys Simulator",
            refreshRateHz: UInt32(refreshRate)
        )
        trackingSender.connect(serverIP: server.address)
        controlChannel.connect(serverIP: server.address)

        startStatsTimer()
        reconfigureTracking()

        state = .streaming
        statusText = "Streaming"
    }

    func disconnect() {
        stopTracking()
        stopStatsTimer()
        videoReceiver.stop()
        trackingSender.disconnect()
        controlChannel.disconnect()
        decoder.invalidate()
        discovery.stop()
        latencyReporter.reset()
        #if os(macOS)
        closeWebcamPreviewWindows()
        #endif

        state = .disconnected
        discoveredServer = nil
        framesDecoded = 0
        isTracking = false
        statusText = {
            #if os(iOS)
            "Tap Search to find the runtime"
            #else
            "Click Search to find the runtime"
            #endif
        }()
    }

    func requestKeyframe() {
        controlChannel.requestKeyframe()
    }

    func resetPose() {
        #if os(macOS)
        if webcamTrackingEnabled && webcamHeadTrackingEnabled {
            desktopWebcamTracking.resetHeadPose()
            return
        }
        #endif
        #if os(iOS)
        guard viewerMode == .stereoView else { return }
        arTracking.resetPose()
        #endif
    }

    private func reconfigureTracking() {
        stopTracking()
        renderer?.displayMode = viewerMode.rendererMode

        switch viewerMode {
        case .simulator:
            startSimulatorTracking()
        case .stereoView:
            #if os(iOS)
            startDeviceTracking()
            #else
            startSimulatorTracking()
            #endif
        }
    }

    private func startSimulatorTracking() {
        isTracking = true
        lastTrackingTimeNs = VideoReceiver.monotonicNs()
        #if os(macOS)
        startDesktopWebcamTrackingIfNeeded()
        #endif

        let source = DispatchSource.makeTimerSource(
            flags: .strict,
            queue: DispatchQueue.global(qos: .userInteractive)
        )
        source.schedule(
            deadline: .now(),
            repeating: .nanoseconds(11_111_111),
            leeway: .microseconds(500)
        )
        source.setEventHandler { [weak self] in
            guard let self else { return }
            let now = VideoReceiver.monotonicNs()
            let dt = Float(now - self.lastTrackingTimeNs) / 1_000_000_000.0
            self.lastTrackingTimeNs = now
            self.inputManager.update(deltaTime: max(dt, 0.001))
            var packet = self.inputManager.buildTrackingPacket()
            #if os(macOS)
            if self.webcamTrackingEnabled {
                packet = WebcamGestureMapper.makeTrackingPacket(
                    basePacket: packet,
                    snapshot: self.loadWebcamSnapshot(),
                    outputMode: self.webcamOutputMode,
                    headTrackingEnabled: self.webcamHeadTrackingEnabled,
                    settings: self.currentWebcamSettings()
                )
            }
            #endif
            self.trackingSender.send(packet)
        }
        source.resume()
        trackingSource = source
    }

    #if os(iOS)
    private func startDeviceTracking() {
        arTracking.onTrackingUpdate = { [weak self] snapshot in
            guard let self else { return }
            self.trackingSender.send(Self.makeTrackingPacket(from: snapshot))
            Task { @MainActor in
                self.isTracking = snapshot.isTracking
            }
        }
        arTracking.start()
    }

    nonisolated private static func makeTrackingPacket(from snapshot: TrackingSnapshot) -> TrackingPacket {
        var packet = TrackingPacket()
        packet.timestampNs = snapshot.timestampNs
        packet.headPosition = (snapshot.position.x, snapshot.position.y, snapshot.position.z)

        let orientation = snapshot.orientation
        packet.headOrientation = (
            orientation.imag.x,
            orientation.imag.y,
            orientation.imag.z,
            orientation.real
        )

        if let left = snapshot.leftHand {
            packet.trackingFlags |= TrackingFlagsValues.leftHandActive
            packet.leftControllerPos = (
                left.wristPosition.x,
                left.wristPosition.y,
                left.wristPosition.z
            )
            packet.leftControllerRot = (
                left.wristRotation.imag.x,
                left.wristRotation.imag.y,
                left.wristRotation.imag.z,
                left.wristRotation.real
            )
            for (index, joint) in left.joints.enumerated() {
                packet.leftHandJoints.setJoint(
                    index: index,
                    x: joint.x,
                    y: joint.y,
                    z: joint.z,
                    radius: 0.01
                )
            }
        }

        if let right = snapshot.rightHand {
            packet.trackingFlags |= TrackingFlagsValues.rightHandActive
            packet.rightControllerPos = (
                right.wristPosition.x,
                right.wristPosition.y,
                right.wristPosition.z
            )
            packet.rightControllerRot = (
                right.wristRotation.imag.x,
                right.wristRotation.imag.y,
                right.wristRotation.imag.z,
                right.wristRotation.real
            )
            for (index, joint) in right.joints.enumerated() {
                packet.rightHandJoints.setJoint(
                    index: index,
                    x: joint.x,
                    y: joint.y,
                    z: joint.z,
                    radius: 0.01
                )
            }
        }

        return packet
    }
    #endif

    private func stopTracking() {
        trackingSource?.cancel()
        trackingSource = nil
        #if os(macOS)
        desktopWebcamTracking.stop()
        storeWebcamSnapshot(nil)
        #endif
        #if os(iOS)
        arTracking.stop()
        #endif
        isTracking = false
    }

    #if os(macOS)
    func refreshWebcamDevices() {
        webcamDevices = DesktopWebcamTrackingManager.availableVideoDevices()
        if !selectedWebcamID.isEmpty && !webcamDevices.contains(where: { $0.id == selectedWebcamID }) {
            selectedWebcamID = ""
        }
        if !secondaryWebcamID.isEmpty &&
            (!webcamDevices.contains(where: { $0.id == secondaryWebcamID }) ||
                secondaryWebcamID == selectedWebcamID) {
            secondaryWebcamID = ""
        }
    }

    func importWebcamCalibration(from url: URL) {
        do {
            let needsScopedAccess = url.startAccessingSecurityScopedResource()
            defer {
                if needsScopedAccess {
                    url.stopAccessingSecurityScopedResource()
                }
            }
            let data = try Data(contentsOf: url)
            let calibration = try JSONDecoder().decode(WebcamRigCalibration.self, from: data)
            guard calibration.isUsable else {
                webcamCalibrationStatusText = "Calibration needs at least two cameras"
                return
            }
            webcamRigCalibration = calibration
            let errorText: String
            if let error = calibration.reprojectionError {
                errorText = String(format: ", %.3f reprojection error", Double(error))
            } else {
                errorText = ""
            }
            webcamCalibrationStatusText = "Loaded \(calibration.cameras.count)-camera calibration\(errorText)"
            restartDesktopWebcamTrackingIfNeeded()
        } catch {
            webcamCalibrationStatusText = "Calibration import failed"
        }
    }

    func resetWebcamCalibration() {
        webcamRigCalibration = nil
        webcamCalibrationStatusText = "No calibration loaded"
        restartDesktopWebcamTrackingIfNeeded()
    }

    func openWebcamPreviewWindows() {
        refreshWebcamDevices()
        let devices = selectedWebcamPreviewDevices()
        webcamPreviewGeneration &+= 1
        webcamPreviewWindows.showWindows(for: devices)
        webcamPreviewWindowsOpen = !devices.isEmpty
        desktopWebcamTracking.setPreviewEnabled(
            webcamPreviewWindowsOpen,
            generation: webcamPreviewGeneration
        )
        if webcamPreviewWindowsOpen {
            startDesktopWebcamTrackingIfNeeded()
        }
    }

    func closeWebcamPreviewWindows() {
        webcamPreviewGeneration &+= 1
        webcamPreviewWindowsOpen = false
        desktopWebcamTracking.setPreviewEnabled(false, generation: webcamPreviewGeneration)
        webcamPreviewWindows.closeAll()
    }

    private func handleWebcamPreviewFrame(_ frame: WebcamPreviewFrame) {
        guard webcamPreviewWindowsOpen, frame.previewGeneration == webcamPreviewGeneration else { return }
        webcamPreviewWindows.show(frame: frame)
    }

    private func selectedWebcamPreviewDevices() -> [DesktopWebcamDevice] {
        guard !webcamDevices.isEmpty else { return [] }
        let primary = webcamDevices.first { $0.id == selectedWebcamID } ?? webcamDevices[0]
        guard webcamSourceMode != .singleCamera else { return [primary] }

        let secondary =
            webcamDevices.first { $0.id == secondaryWebcamID && $0.id != primary.id } ??
            webcamDevices.first { $0.id != primary.id }

        if let secondary {
            return [primary, secondary]
        }
        return [primary]
    }

    private func startDesktopWebcamTrackingIfNeeded() {
        guard webcamTrackingEnabled else {
            desktopWebcamTracking.stop()
            storeWebcamSnapshot(nil)
            return
        }

        refreshWebcamDevices()
        updateDesktopWebcamSettings()
        storeWebcamSnapshot(nil)
        webcamStatusText = "Starting webcam tracking"
        desktopWebcamTracking.start(
            configuration: DesktopWebcamTrackingConfiguration(
                sourceMode: webcamSourceMode,
                primaryDeviceID: selectedWebcamID,
                secondaryDeviceID: secondaryWebcamID,
                captureResolution: webcamCaptureResolution,
                calibration: webcamRigCalibration
            )
        )
    }

    private func restartDesktopWebcamTrackingIfNeeded() {
        guard webcamTrackingEnabled else { return }
        startDesktopWebcamTrackingIfNeeded()
    }

    private func updateDesktopWebcamSettings() {
        desktopWebcamTracking.updateSettings(currentWebcamSettings())
    }

    private func currentWebcamSettings() -> WebcamTrackingSettings {
        WebcamTrackingSettings(
            trackingSpace: webcamTrackingSpace,
            cameraFacing: webcamCameraFacing,
            cameraY: webcamCameraY,
            headOffset: SIMD3<Float>(
                webcamHeadOffsetX,
                webcamHeadOffsetY,
                webcamHeadOffsetZ
            ),
            handYOffset: webcamHandYOffset,
            headPositionInterpolation: SIMD3<Float>(
                webcamHeadPositionInterpolationX,
                webcamHeadPositionInterpolationY,
                webcamHeadPositionInterpolationZ
            ),
            headRotationInterpolation: SIMD3<Float>(
                webcamHeadRotationInterpolationYaw,
                webcamHeadRotationInterpolationPitch,
                webcamHeadRotationInterpolationRoll
            ),
            handPositionInterpolation: SIMD3<Float>(
                webcamHandPositionInterpolationX,
                webcamHandPositionInterpolationY,
                webcamHandPositionInterpolationZ
            ),
            handRotationInterpolation: webcamHandRotationInterpolation,
            handDepthScale: webcamHandDepthScale,
            handDepthOffset: webcamHandDepthOffset,
            handDepthSmoothing: webcamHandDepthSmoothing,
            movementDeadzone: webcamMovementDeadzone,
            headRotationDeadzoneDegrees: webcamHeadRotationDeadzoneDegrees,
            headRotationLimitDegrees: SIMD3<Float>(
                webcamHeadRotationLimitYawDegrees,
                webcamHeadRotationLimitPitchDegrees,
                webcamHeadRotationLimitRollDegrees
            ),
            controllerRotationOffsetDegrees: SIMD3<Float>(
                webcamControllerRotationXDegrees,
                webcamControllerRotationYDegrees,
                webcamControllerRotationZDegrees
            )
        )
    }

    private func storeWebcamSnapshot(_ snapshot: WebcamTrackingSnapshot?) {
        webcamSnapshotLock.lock()
        latestWebcamSnapshot = snapshot
        webcamSnapshotLock.unlock()
    }

    private func loadWebcamSnapshot() -> WebcamTrackingSnapshot? {
        webcamSnapshotLock.lock()
        let snapshot = latestWebcamSnapshot
        webcamSnapshotLock.unlock()
        return snapshot
    }
    #endif

    nonisolated private static func nanoseconds(from time: CMTime) -> Int64 {
        guard time.isValid else { return 0 }
        return CMTimeConvertScale(time, timescale: 1_000_000_000, method: .default).value
    }

    private func startStatsTimer() {
        lastStatsTime = VideoReceiver.monotonicNs()
        lastStatsFramesDelivered = 0
        statsTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.updateStats()
            }
        }
    }

    private func stopStatsTimer() {
        statsTimer?.invalidate()
        statsTimer = nil
    }

    @MainActor
    private func updateStats() {
        let now = VideoReceiver.monotonicNs()
        let dtSec = Double(now - lastStatsTime) / 1_000_000_000.0
        guard dtSec > 0 else { return }

        let delivered = videoReceiver.framesDelivered
        let deltaDelivered = delivered - lastStatsFramesDelivered

        stats.packetsReceived = videoReceiver.packetsReceived
        stats.framesDelivered = delivered
        stats.framesDropped = videoReceiver.framesDropped
        stats.totalFramesSeen = videoReceiver.totalFramesSeen
        stats.decodeErrors = decoder.totalDecodeErrors
        stats.deliveryFps = Double(deltaDelivered) / dtSec

        if stats.totalFramesSeen > 0 {
            stats.lossPercent = Double(stats.framesDropped) / Double(stats.totalFramesSeen) * 100.0
        }

        lastStatsTime = now
        lastStatsFramesDelivered = delivered

        let currentDropped = videoReceiver.framesDropped
        if currentDropped > lastObservedDroppedFrames {
            let nowMono = UInt64(now)
            if nowMono - lastKeyframeRequestTime > keyframeRequestCooldownNs {
                lastKeyframeRequestTime = nowMono
                controlChannel.requestKeyframe(
                    reason: KeyframeReason.frameLoss.rawValue,
                    detail: UInt32(currentDropped - lastObservedDroppedFrames)
                )
            }
        }
        lastObservedDroppedFrames = currentDropped

        let lastPacket = videoReceiver.lastPacketReceivedTimeNs
        if lastPacket > 0 && framesDecoded > 0 {
            let stallMs = Double(now - lastPacket) / 1_000_000.0
            if stallMs > 2000.0 {
                let nowMono = UInt64(now)
                if nowMono - lastKeyframeRequestTime > keyframeRequestCooldownNs {
                    lastKeyframeRequestTime = nowMono
                    controlChannel.requestKeyframe(
                        reason: KeyframeReason.decodeStall.rawValue,
                        detail: UInt32(stallMs)
                    )
                }
            }
        }
    }
}
