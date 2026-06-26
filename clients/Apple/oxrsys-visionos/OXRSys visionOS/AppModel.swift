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

private final class PixelBufferState: @unchecked Sendable {
    private let lock = NSLock()
    private var pixelBuffer: CVPixelBuffer?
    private var presentationTimeNs: Int64 = 0

    func set(_ newValue: CVPixelBuffer?, presentationTimeNs: Int64) {
        lock.lock()
        pixelBuffer = newValue
        self.presentationTimeNs = presentationTimeNs
        lock.unlock()
    }

    func get() -> CVPixelBuffer? {
        lock.lock()
        let value = pixelBuffer
        lock.unlock()
        return value
    }

    /// The displayed frame and the presentation timestamp it was tagged with, read together so
    /// the renderer reprojects that exact frame with the render pose that belongs to it.
    func getWithTimestamp() -> (pixelBuffer: CVPixelBuffer?, presentationTimeNs: Int64) {
        lock.lock()
        defer { lock.unlock() }
        return (pixelBuffer, presentationTimeNs)
    }
}

private final class KeyframeRecoveryState: @unchecked Sendable {
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

private final class EyeProjectionState: @unchecked Sendable {
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

/// Stores the head orientation the server rendered each frame for, keyed by the frame's
/// presentation timestamp, so the renderer can reproject the displayed frame to the live pose.
private final class RenderPoseReprojector: @unchecked Sendable {
    private let lock = NSLock()
    private var orientationByPresentationNs: [Int64: simd_quatf] = [:]
    private let capacity = 240   // ring-buffer cap (~a few seconds of frames); bounds memory only
    private var latestKey: Int64 = 0
    private var latestOrientation: simd_quatf?

    func note(presentationTimeNs: Int64, orientation: simd_quatf) {
        lock.lock()
        orientationByPresentationNs[presentationTimeNs] = orientation
        if presentationTimeNs >= latestKey {
            latestKey = presentationTimeNs
            latestOrientation = orientation
        }
        if orientationByPresentationNs.count > capacity,
           let oldest = orientationByPresentationNs.keys.min() {
            orientationByPresentationNs.removeValue(forKey: oldest)
        }
        lock.unlock()
    }

    /// Exact render pose for this frame, falling back to the most recent one if this frame's pose
    /// packet was lost (it's a single un-FEC'd UDP packet) — exact match first, recent pose second.
    func orientation(forPresentationTimeNs presentationTimeNs: Int64) -> simd_quatf? {
        lock.lock()
        defer { lock.unlock() }
        return orientationByPresentationNs[presentationTimeNs] ?? latestOrientation
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
    var connectionState: ConnectionState = .disconnected
    var discoveredServer: DiscoveredServer?
    var statusText = "Tap Search to find the runtime"
    var framesDecoded: UInt64 = 0
    var isTrackingActive = false
    var stats = StreamStats()
    var showStats = true

    private let discovery = DiscoveryClient()
    private let videoReceiver = VideoReceiver()
    private let trackingSender = TrackingSender()
    private let controlChannel = ControlChannel()
    private let decoder = H265Decoder()
    private let latencyReporter = LatencyReporter()
    private let trackingManager = VisionTrackingManager()
    private nonisolated let pixelBufferState = PixelBufferState()
    private nonisolated let keyframeRecoveryState = KeyframeRecoveryState()
    private nonisolated let eyeProjectionState = EyeProjectionState()
    private nonisolated let renderPoseReprojector = RenderPoseReprojector()

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
                self.statusText = "Found \(server.name), connecting..."
                self.connect()
            }
        }
    }

    func connect() {
        guard let server = discoveredServer else { return }
        let serverAddress = resolvedServerAddress(for: server)
        let refreshRateHz = refreshRate

        connectionState = .connecting
        statusText = "Connecting to \(server.name)..."

        let keyframeErrorThreshold = keyframeErrorThreshold
        let keyframeRequestCooldownNs = keyframeRequestCooldownNs
        decoder.configure { [weak self] pixelBuffer, presentationTime in
            guard let self else { return }
            self.keyframeRecoveryState.noteDecodedFrame()
            self.pixelBufferState.set(pixelBuffer, presentationTimeNs: Self.nanoseconds(from: presentationTime))

            self.latencyReporter.noteFrameDecoded(
                presentationTimeNs: Self.nanoseconds(from: presentationTime),
                decodeTimeNs: VideoReceiver.monotonicNs(),
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

        videoReceiver.start(onNalUnit: { [weak self] nalData, presentationTimeNs, receiveTimeNs in
            guard let self else { return }
            self.latencyReporter.noteFrameReceived(
                presentationTimeNs: presentationTimeNs,
                receiveTimeNs: receiveTimeNs
            )
            self.decoder.decode(nalData: nalData, presentationTimeNs: presentationTimeNs)
        }, onRenderPose: { [weak self] presentationTimeNs, orientation in
            let quat = simd_quatf(ix: orientation.0, iy: orientation.1, iz: orientation.2, r: orientation.3)
            self?.renderPoseReprojector.note(presentationTimeNs: presentationTimeNs, orientation: quat)
        })

        Thread.sleep(forTimeInterval: 0.05)

        let connectionServer = DiscoveredServer(announce: server.announce, address: serverAddress)
        discovery.sendConnect(
            to: connectionServer,
            deviceName: "OXRSys visionOS",
            refreshRateHz: UInt32(refreshRateHz)
        )
        trackingSender.connect(serverIP: serverAddress)
        controlChannel.connect(serverIP: serverAddress)

        startStatsTimer()
        updateTrackingState()

        connectionState = .streaming
        statusText = "Streaming from \(server.name) via \(serverAddress)"
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
        discoveredServer = nil
        framesDecoded = 0
        isTrackingActive = false
        stats = StreamStats()
        statusText = "Tap Search to find the runtime"
        pixelBufferState.set(nil, presentationTimeNs: 0)
        keyframeRecoveryState.reset()
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
    nonisolated func currentFrame() -> (pixelBuffer: CVPixelBuffer?, presentationTimeNs: Int64) {
        pixelBufferState.getWithTimestamp()
    }

    /// The head orientation the frame with this presentation timestamp was rendered for, used by
    /// the renderer to reproject it to the live head pose.
    nonisolated func renderOrientation(forPresentationTimeNs presentationTimeNs: Int64) -> simd_quatf? {
        renderPoseReprojector.orientation(forPresentationTimeNs: presentationTimeNs)
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
