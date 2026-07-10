// SPDX-License-Identifier: MPL-2.0

import ARKit
import CoreMedia
import CoreVideo
import Foundation
import Observation
import OXRSysStreaming
import os
import QuartzCore
import SwiftUI
import simd

private nonisolated final class PixelBufferState: @unchecked Sendable {
    private let lock = NSLock()
    private var pixelBuffer: CVPixelBuffer?
    private var presentationTimeNs: Int64 = 0
    private var decodeTimeNs: Int64 = 0

    func set(_ newValue: CVPixelBuffer?, presentationTimeNs: Int64, decodeTimeNs: Int64) {
        lock.lock()
        pixelBuffer = newValue
        self.presentationTimeNs = presentationTimeNs
        self.decodeTimeNs = decodeTimeNs
        lock.unlock()
    }

    func get() -> CVPixelBuffer? {
        lock.lock()
        let value = pixelBuffer
        lock.unlock()
        return value
    }

    /// The displayed frame, the presentation timestamp it was tagged with, and the wall time it
    /// finished decoding — read together so the renderer reprojects that exact frame with the
    /// render pose that belongs to it and can measure real decode-to-photon latency.
    func getWithTimestamp() -> (pixelBuffer: CVPixelBuffer?, presentationTimeNs: Int64, decodeTimeNs: Int64) {
        lock.lock()
        defer { lock.unlock() }
        return (pixelBuffer, presentationTimeNs, decodeTimeNs)
    }
}

private nonisolated final class KeyframeRecoveryState: @unchecked Sendable {
    private let lock = NSLock()
    private var consecutiveDecodeErrors = 0
    private var lastKeyframeRequestTime: UInt64 = 0

    func reset() {
        lock.lock()
        consecutiveDecodeErrors = 0
        lastKeyframeRequestTime = 0
        lock.unlock()
    }

    func noteDecodedFrame() {
        lock.lock()
        consecutiveDecodeErrors = 0
        lock.unlock()
    }

    func shouldRequestKeyframe(now: UInt64, threshold: Int, cooldownNs: UInt64) -> Bool {
        lock.lock()
        defer { lock.unlock() }

        consecutiveDecodeErrors += 1
        guard consecutiveDecodeErrors >= threshold else {
            return false
        }

        guard now - lastKeyframeRequestTime > cooldownNs else {
            return false
        }

        lastKeyframeRequestTime = now
        consecutiveDecodeErrors = 0
        return true
    }
}

private nonisolated final class EyeProjectionState: @unchecked Sendable {
    private let lock = NSLock()
    private var fovAngles = SIMD4<Float>(repeating: 0) // angleLeft, angleRight, angleUp, angleDown (radians)
    private var ipd: Float = 0

    func set(fovAngles: SIMD4<Float>, ipd: Float) {
        lock.lock()
        self.fovAngles = fovAngles
        self.ipd = ipd
        lock.unlock()
    }

    func get() -> (fovAngles: SIMD4<Float>, ipd: Float) {
        lock.lock()
        defer { lock.unlock() }
        return (fovAngles, ipd)
    }
}

/// The full head pose (position + orientation) the server rendered a frame for.
nonisolated struct RenderPose: Sendable {
    var position: SIMD3<Float>
    var orientation: simd_quatf
}

/// Stores the head pose the server rendered each frame for, keyed by the frame's presentation
/// timestamp, so the renderer can reproject the displayed frame to the live pose — rotation
/// exactly, and translation against the reprojection depth plane.
private nonisolated final class RenderPoseReprojector: @unchecked Sendable {
    private let lock = NSLock()
    private var poseByPresentationNs: [Int64: RenderPose] = [:]
    private let capacity = 240   // ring-buffer cap (~a few seconds of frames); bounds memory only
    private var latestKey: Int64 = 0
    private var latestPose: RenderPose?

    func note(presentationTimeNs: Int64, pose: RenderPose) {
        lock.lock()
        poseByPresentationNs[presentationTimeNs] = pose
        if presentationTimeNs >= latestKey {
            latestKey = presentationTimeNs
            latestPose = pose
        }
        if poseByPresentationNs.count > capacity,
           let oldest = poseByPresentationNs.keys.min() {
            poseByPresentationNs.removeValue(forKey: oldest)
        }
        lock.unlock()
    }

    /// Exact render pose for this frame, falling back to the most recent one if this frame's pose
    /// packet was lost (it's a single un-FEC'd UDP packet) — exact match first, recent pose second.
    func pose(forPresentationTimeNs presentationTimeNs: Int64) -> RenderPose? {
        lock.lock()
        defer { lock.unlock() }
        return poseByPresentationNs[presentationTimeNs] ?? latestPose
    }
}

/// Foveated-encoding (AADT) parameters handed to the fragment shader. Memory layout must match
/// the Metal `FoveationParams` struct. `enabled == 0` is a passthrough.
nonisolated struct FoveationShaderParams {
    var enabled: UInt32 = 0
    var pad: UInt32 = 0
    var centerSize: SIMD2<Float> = SIMD2<Float>(1, 1)
    var centerShift: SIMD2<Float> = SIMD2<Float>(0, 0)
    var edgeRatio: SIMD2<Float> = SIMD2<Float>(1, 1)
    var eyeSizeRatio: SIMD2<Float> = SIMD2<Float>(1, 1)
}

/// Holds the foveation parameters for the renderer to read each frame. Computed once per
/// connection from the server announce (dynamic reconfiguration is not handled yet).
private nonisolated final class FoveationState: @unchecked Sendable {
    private let lock = NSLock()
    private var params = FoveationShaderParams()

    func set(_ newValue: FoveationShaderParams) {
        lock.lock()
        params = newValue
        lock.unlock()
    }

    func get() -> FoveationShaderParams {
        lock.lock()
        defer { lock.unlock() }
        return params
    }
}

/// Headset contrast-adaptive sharpening strength (0–1) for the renderer, set from the server
/// announce. 0 = off.
private nonisolated final class PostFXState: @unchecked Sendable {
    private let lock = NSLock()
    private var sharpen: Float = 0

    func setSharpen(_ value: Float) {
        lock.lock()
        sharpen = value
        lock.unlock()
    }

    func sharpenValue() -> Float {
        lock.lock()
        defer { lock.unlock() }
        return sharpen
    }
}

@MainActor
@Observable
final class AppModel {
    enum ConnectionState {
        case disconnected
        case discovering
        case connecting
        case streaming
    }

    enum ImmersiveSpaceState {
        case closed
        case inTransition
        case open
    }

    struct StreamStats {
        var packetsReceived: UInt32 = 0
        var framesDelivered: UInt32 = 0
        var framesDropped: UInt32 = 0
        var totalFramesSeen: UInt32 = 0
        var decodeErrors: Int = 0
        var deliveryFps: Double = 0
    }

    let immersiveSpaceID = "ImmersiveSpace"
    let controlWindowID = "ControlWindow"

    var immersiveSpaceState = ImmersiveSpaceState.closed
    /// User intent to be in the immersive view, kept separate from `connectionState` so that
    /// exiting the immersive view (Digital Crown) lands on the menu instead of auto re-entering,
    /// and the menu can offer an explicit "Enter" button while still connected.
    var wantsImmersiveSpace = false
    var autoEnterImmersiveOnConnect = true
    var showHandsInImmersive = true
    var keepControlWindowVisibleInImmersive = false
    var shouldRestoreControlWindowOnImmersiveClose = false
    var connectionState: ConnectionState = .disconnected
    var discoveredServer: DiscoveredServer?
    var statusText = "Tap Search to find the runtime"
    var framesDecoded: UInt64 = 0
    var isTrackingActive = false
    var stats = StreamStats()
    var showStats = true

    /// Emulate VR controllers from hand tracking (pinch/curl gestures) and, when an Xbox-style
    /// gamepad is connected, from hand pose + gamepad buttons — so controller-only PCVR games are
    /// playable without physical spatial controllers. Off by default.
    var emulateControllers = false {
        didSet {
            trackingManager.setGestureEmulationEnabled(emulateControllers)
            trackingManager.setControllerCompatibilityEnabled(emulateControllers)
        }
    }

    private let discovery = DiscoveryClient()
    private let videoReceiver = VideoReceiver()
    private let trackingSender = TrackingSender()
    private let controlChannel = ControlChannel()
    private let decoder = VideoDecoder()
    private let latencyReporter = LatencyReporter()
    private let trackingManager = VisionTrackingManager()
    private nonisolated let pixelBufferState = PixelBufferState()
    private nonisolated let keyframeRecoveryState = KeyframeRecoveryState()
    private nonisolated let eyeProjectionState = EyeProjectionState()
    private nonisolated let renderPoseReprojector = RenderPoseReprojector()
    private nonisolated let foveationState = FoveationState()
    private nonisolated let postFXState = PostFXState()

    private var statsTimer: Timer?
    private var lastStatsTimeNs: Int64 = 0
    private var lastDeliveredFrames: UInt32 = 0
    private let keyframeErrorThreshold = 3
    private let keyframeRequestCooldownNs: UInt64 = 1_000_000_000

    init() {
        trackingManager.onTrackingUpdate = { [weak self] snapshot in
            guard let self else { return }

            var packet = TrackingPacket()
            packet.timestampNs = snapshot.timestampNs
            packet.headPosition = (
                snapshot.position.x,
                snapshot.position.y,
                snapshot.position.z
            )
            packet.headOrientation = (
                snapshot.orientation.imag.x,
                snapshot.orientation.imag.y,
                snapshot.orientation.imag.z,
                snapshot.orientation.real
            )
            // Measured head velocities: the runtime prefers these for its bounded pose
            // prediction over finite-differencing the received poses.
            packet.headLinearVelocity = (
                snapshot.linearVelocity.x,
                snapshot.linearVelocity.y,
                snapshot.linearVelocity.z
            )
            packet.headAngularVelocity = (
                snapshot.angularVelocity.x,
                snapshot.angularVelocity.y,
                snapshot.angularVelocity.z
            )
            let eyeProjection = self.eyeProjectionState.get()
            packet.ipd = eyeProjection.ipd > 0 ? eyeProjection.ipd : 0.064
            if eyeProjection.fovAngles != SIMD4<Float>(repeating: 0) {
                packet.eyeFov = (
                    eyeProjection.fovAngles.x,
                    eyeProjection.fovAngles.y,
                    eyeProjection.fovAngles.z,
                    eyeProjection.fovAngles.w
                )
            }

            if let leftHand = snapshot.leftHand {
                packet.trackingFlags |= TrackingFlagsValues.leftHandActive
                packet.leftControllerPos = (
                    leftHand.wristPosition.x,
                    leftHand.wristPosition.y,
                    leftHand.wristPosition.z
                )
                packet.leftControllerRot = (
                    leftHand.wristRotation.imag.x,
                    leftHand.wristRotation.imag.y,
                    leftHand.wristRotation.imag.z,
                    leftHand.wristRotation.real
                )

                for (index, joint) in leftHand.joints.enumerated() {
                    packet.leftHandJoints.setJoint(
                        index: index,
                        x: joint.x,
                        y: joint.y,
                        z: joint.z,
                        radius: 0.01
                    )
                }
            }

            if let rightHand = snapshot.rightHand {
                packet.trackingFlags |= TrackingFlagsValues.rightHandActive
                packet.rightControllerPos = (
                    rightHand.wristPosition.x,
                    rightHand.wristPosition.y,
                    rightHand.wristPosition.z
                )
                packet.rightControllerRot = (
                    rightHand.wristRotation.imag.x,
                    rightHand.wristRotation.imag.y,
                    rightHand.wristRotation.imag.z,
                    rightHand.wristRotation.real
                )

                for (index, joint) in rightHand.joints.enumerated() {
                    packet.rightHandJoints.setJoint(
                        index: index,
                        x: joint.x,
                        y: joint.y,
                        z: joint.z,
                        radius: 0.01
                    )
                }
            }

            if let leftController = snapshot.leftController {
                packet.trackingFlags |= TrackingFlagsValues.leftControllerActive
                packet.leftControllerPos = (
                    leftController.position.x,
                    leftController.position.y,
                    leftController.position.z
                )
                packet.leftControllerRot = (
                    leftController.orientation.imag.x,
                    leftController.orientation.imag.y,
                    leftController.orientation.imag.z,
                    leftController.orientation.real
                )
                packet.buttonState |= leftController.buttonState
                packet.leftTrigger = leftController.trigger
                packet.leftGrip = leftController.grip
                packet.leftThumbstick = (
                    leftController.thumbstick.x,
                    leftController.thumbstick.y
                )
            }

            if let rightController = snapshot.rightController {
                packet.trackingFlags |= TrackingFlagsValues.rightControllerActive
                packet.rightControllerPos = (
                    rightController.position.x,
                    rightController.position.y,
                    rightController.position.z
                )
                packet.rightControllerRot = (
                    rightController.orientation.imag.x,
                    rightController.orientation.imag.y,
                    rightController.orientation.imag.z,
                    rightController.orientation.real
                )
                packet.buttonState |= rightController.buttonState
                packet.rightTrigger = rightController.trigger
                packet.rightGrip = rightController.grip
                packet.rightThumbstick = (
                    rightController.thumbstick.x,
                    rightController.thumbstick.y
                )
            }

            trackingSender.send(packet)

            Task { @MainActor [weak self] in
                self?.isTrackingActive = snapshot.isTracking
            }
        }
    }

    var refreshRate: Int {
        Int(discoveredServer?.refreshRate ?? 90)
    }

    /// Shared with the immersive renderer so device anchors come from one running ARKit
    /// session; two separate world-tracking providers conflict and return nil anchors.
    var sharedWorldTracking: WorldTrackingProvider {
        trackingManager.worldTracking
    }

    func startDiscovery() {
        guard connectionState == .disconnected else { return }
        connectionState = .discovering
        discoveredServer = nil
        statusText = "Searching for server..."

        discovery.start { [weak self] server in
            Task { @MainActor [weak self] in
                guard let self, self.connectionState == .discovering else { return }
                self.discoveredServer = server
                self.discovery.stop()
                if self.autoEnterImmersiveOnConnect {
                    self.statusText = "Found \(server.name), connecting..."
                    self.connect()
                } else {
                    self.connectionState = .disconnected
                    self.statusText = "Found \(server.name)"
                }
            }
        }
    }

    func connect() {
        guard let server = discoveredServer else { return }
        let serverAddress = resolvedServerAddress(for: server)
        let refreshRateHz = refreshRate

        // Fresh ARKit session/providers for this connection — single-use providers can't be
        // re-run after a previous disconnect. Must happen before the immersive space opens so
        // the renderer captures the live world-tracking provider.
        trackingManager.prepareForNewSession()

        connectionState = .connecting
        statusText = "Connecting to \(server.name)..."

        let keyframeErrorThreshold = keyframeErrorThreshold
        let keyframeRequestCooldownNs = keyframeRequestCooldownNs
        decoder.setPrefer10Bit(true)
        decoder.configure { [weak self] pixelBuffer, presentationTime in
            guard let self else { return }
            self.keyframeRecoveryState.noteDecodedFrame()
            let decodeTimeNs = VideoReceiver.monotonicNs()
            self.pixelBufferState.set(pixelBuffer,
                                      presentationTimeNs: Self.nanoseconds(from: presentationTime),
                                      decodeTimeNs: decodeTimeNs)

            self.latencyReporter.noteFrameDecoded(
                presentationTimeNs: Self.nanoseconds(from: presentationTime),
                decodeTimeNs: decodeTimeNs,
                refreshRateHz: refreshRateHz,
                controlChannel: self.controlChannel
            )

            Task { @MainActor [weak self] in
                guard let self else { return }
                self.framesDecoded += 1
            }
        }

        decoder.onDecodeError = { [weak self] in
            guard let self else { return }
            let now = UInt64(VideoReceiver.monotonicNs())
            if self.keyframeRecoveryState.shouldRequestKeyframe(
                now: now,
                threshold: keyframeErrorThreshold,
                cooldownNs: keyframeRequestCooldownNs
            ) {
                self.controlChannel.requestKeyframe(reason: KeyframeReason.decodeStall.rawValue)
            }
        }

        videoReceiver.startReceivingEncoded(onNalUnit: { [weak self] frame in
            guard let self else { return }
            self.latencyReporter.noteFrameReceived(
                presentationTimeNs: frame.presentationTimeNs,
                receiveTimeNs: frame.receiveTimeNs
            )
            self.decoder.decode(
                nalData: frame.data,
                codec: frame.codec,
                presentationTimeNs: frame.presentationTimeNs
            )
        }, onRenderPose: { [weak self] presentationTimeNs, position, orientation in
            let pose = RenderPose(
                position: SIMD3<Float>(position.0, position.1, position.2),
                orientation: simd_quatf(ix: orientation.0, iy: orientation.1, iz: orientation.2, r: orientation.3)
            )
            self?.renderPoseReprojector.note(presentationTimeNs: presentationTimeNs, pose: pose)
        })

        Thread.sleep(forTimeInterval: 0.05)

        // The server foveates and 10-bit-encodes only when the client advertises it can decode
        // the result; compute the inverse-warp params from what the server announces it will send.
        foveationState.set(Self.foveationParams(announce: server.announce, enabled: true))
        postFXState.setSharpen(Self.sharpenStrength(from: server.announce))

        let connectionServer = DiscoveredServer(announce: server.announce, address: serverAddress)
        discovery.sendConnect(
            to: connectionServer,
            deviceName: "OXRSys visionOS",
            refreshRateHz: UInt32(refreshRateHz),
            clientCapabilities: ClientCapabilityFlags.tenBitEncoding | ClientCapabilityFlags.foveatedEncoding
        )
        trackingSender.connect(serverIP: serverAddress)
        controlChannel.connect(serverIP: serverAddress)

        startStatsTimer()
        updateTrackingState()

        connectionState = .streaming
        wantsImmersiveSpace = autoEnterImmersiveOnConnect
        statusText = "Streaming from \(server.name) via \(serverAddress)"
    }

    /// Re-enter the immersive view from the menu while already connected (server already found).
    func enterImmersiveSpace() {
        guard connectionState == .streaming else { return }
        // Recreate tracking providers before reopening so head tracking resumes — the previous
        // providers stopped when the immersive space closed and can't be re-run.
        trackingManager.prepareForNewSession()
        wantsImmersiveSpace = true
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

        connectionState = .disconnected
        wantsImmersiveSpace = false
        discoveredServer = nil
        framesDecoded = 0
        isTrackingActive = false
        stats = StreamStats()
        statusText = "Tap Search to find the runtime"
        pixelBufferState.set(nil, presentationTimeNs: 0, decodeTimeNs: 0)
        keyframeRecoveryState.reset()
        foveationState.set(FoveationShaderParams())
        postFXState.setSharpen(0)
    }

    /// Foveated-encoding parameters for the fragment shader's inverse warp. Passthrough unless the
    /// server is sending a foveated stream.
    nonisolated func foveationParams() -> FoveationShaderParams {
        foveationState.get()
    }

    /// Current headset sharpening strength (0–1) for the renderer's post-process pass.
    nonisolated func sharpenStrength() -> Float {
        postFXState.sharpenValue()
    }

    /// Sharpening strength (0–1) the server requested via its Home config, carried in the announce
    /// as a percent. Defaults to 0 (off) for older servers that leave the field zero.
    nonisolated private static func sharpenStrength(from announce: ServerAnnounce) -> Float {
        Float(min(announce.clientSharpeningPercent, 100)) / 100.0
    }

    /// Builds the inverse-AADT shader parameters from the server announce so the un-warp matches
    /// the server's encoded layout. Passthrough unless the client opted in and the server is
    /// actually foveating (edge ratios > 1).
    nonisolated private static func foveationParams(announce: ServerAnnounce,
                                                    enabled: Bool) -> FoveationShaderParams {
        var params = FoveationShaderParams()
        let serverFoveating = (announce.serverFeatures & ServerFeatureFlags.foveatedEncoding) != 0
        let edgeX = announce.foveationEdgeRatioX
        let edgeY = announce.foveationEdgeRatioY
        guard enabled, serverFoveating, edgeX > 1, edgeY > 1 else { return params }

        let renderW = Float(max(announce.renderWidth / 2, 1))
        let renderH = Float(max(announce.renderHeight, 1))
        let encodedFullW = announce.encodedWidth > 0 ? announce.encodedWidth : announce.renderWidth
        let encodedFullH = announce.encodedHeight > 0 ? announce.encodedHeight : announce.renderHeight
        let encW = Float(max(encodedFullW / 2, 1))
        let encH = Float(max(encodedFullH, 1))

        func activeRatio(_ target: Float, _ encoded: Float, _ centerSize: Float, _ edgeRatio: Float) -> Float {
            guard target > 0, encoded > 0, edgeRatio > 1 else { return 1 }
            let scale = centerSize + (1 - centerSize) / edgeRatio
            return min(max(scale * target / encoded, 0.0001), 1)
        }

        params.enabled = 1
        params.centerSize = SIMD2<Float>(announce.foveationCenterSizeX, announce.foveationCenterSizeY)
        params.centerShift = SIMD2<Float>(announce.foveationCenterShiftX, announce.foveationCenterShiftY)
        params.edgeRatio = SIMD2<Float>(edgeX, edgeY)
        params.eyeSizeRatio = SIMD2<Float>(
            activeRatio(renderW, encW, announce.foveationCenterSizeX, edgeX),
            activeRatio(renderH, encH, announce.foveationCenterSizeY, edgeY)
        )
        return params
    }

    func requestKeyframe() {
        controlChannel.requestKeyframe()
    }

    func immersiveSpaceDidOpen() {
        immersiveSpaceState = .open
        updateTrackingState()
    }

    func immersiveSpaceDidClose() {
        immersiveSpaceState = .closed
        // The view was dismissed (e.g. Digital Crown); drop the intent so we settle on the
        // menu and let the user re-enter explicitly instead of immediately reopening.
        wantsImmersiveSpace = false
        updateTrackingState()
    }

    private func updateTrackingState() {
        let shouldTrack = connectionState == .streaming && immersiveSpaceState == .open
        if shouldTrack {
            trackingManager.start()
        } else {
            stopTracking()
        }
    }

    private func stopTracking() {
        trackingManager.stop()
        isTrackingActive = false
    }

    private func startStatsTimer() {
        stopStatsTimer()
        lastStatsTimeNs = VideoReceiver.monotonicNs()
        lastDeliveredFrames = videoReceiver.framesDelivered

        statsTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.refreshStats()
            }
        }
    }

    private func stopStatsTimer() {
        statsTimer?.invalidate()
        statsTimer = nil
    }

    private func refreshStats() {
        stats.packetsReceived = videoReceiver.packetsReceived
        stats.framesDelivered = videoReceiver.framesDelivered
        stats.framesDropped = videoReceiver.framesDropped
        stats.totalFramesSeen = videoReceiver.totalFramesSeen
        stats.decodeErrors = decoder.totalDecodeErrors

        let now = VideoReceiver.monotonicNs()
        let deltaNs = now - lastStatsTimeNs
        if deltaNs > 0 {
            let deltaFrames = videoReceiver.framesDelivered - lastDeliveredFrames
            stats.deliveryFps = Double(deltaFrames) * 1_000_000_000.0 / Double(deltaNs)
            lastStatsTimeNs = now
            lastDeliveredFrames = videoReceiver.framesDelivered
        }
    }

    nonisolated func currentPixelBuffer() -> CVPixelBuffer? {
        pixelBufferState.get()
    }

    /// The displayed frame and its presentation timestamp, snapshotted together so the renderer
    /// reprojects that exact frame with the render pose that belongs to it.
    nonisolated func currentFrame() -> (pixelBuffer: CVPixelBuffer?, presentationTimeNs: Int64, decodeTimeNs: Int64) {
        pixelBufferState.getWithTimestamp()
    }

    /// Called by the renderer the first time a decoded frame is drawn, with the measured
    /// decode-to-photon time. Replaces the guessed one-frame compositor budget in the latency
    /// report, so the server's prediction horizon reflects the true client display path.
    nonisolated func noteFrameDisplayed(displayLatencyMs: Double) {
        latencyReporter.noteFrameDisplayed(displayLatencyMs: displayLatencyMs)
    }

    /// The head pose the frame with this presentation timestamp was rendered for, used by the
    /// renderer to reproject it to the live head pose.
    nonisolated func renderPose(forPresentationTimeNs presentationTimeNs: Int64) -> RenderPose? {
        renderPoseReprojector.pose(forPresentationTimeNs: presentationTimeNs)
    }

    /// Called from the render loop with the device's real per-eye FOV (radians, OpenXR
    /// signed angles) and IPD, forwarded to the runtime so it renders the matching frustum.
    nonisolated func updateEyeProjection(fovAngles: SIMD4<Float>, ipd: Float) {
        eyeProjectionState.set(fovAngles: fovAngles, ipd: ipd)
    }

    private func resolvedServerAddress(for server: DiscoveredServer) -> String {
        #if targetEnvironment(simulator)
        return "127.0.0.1"
        #else
        return server.address
        #endif
    }

    nonisolated private static func nanoseconds(from time: CMTime) -> Int64 {
        guard time.isValid else { return 0 }
        return CMTimeConvertScale(time, timescale: 1_000_000_000, method: .default).value
    }
}
