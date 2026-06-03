// SPDX-License-Identifier: MPL-2.0

import CoreMedia
import CoreVideo
import Observation
import OXRSysStreaming
import os
import QuartzCore
import SwiftUI
import simd

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
    private nonisolated(unsafe) var pixelBufferLock = os_unfair_lock()
    private nonisolated(unsafe) var latestPixelBuffer: CVPixelBuffer?

    private var statsTimer: Timer?
    private var lastStatsTimeNs: Int64 = 0
    private var lastDeliveredFrames: UInt32 = 0
    private nonisolated(unsafe) var consecutiveDecodeErrors = 0
    private nonisolated(unsafe) var lastKeyframeRequestTime: UInt64 = 0
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
            packet.ipd = 0.064

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

        decoder.configure { [weak self] pixelBuffer, presentationTime in
            guard let self else { return }
            self.consecutiveDecodeErrors = 0

            os_unfair_lock_lock(&self.pixelBufferLock)
            self.latestPixelBuffer = pixelBuffer
            os_unfair_lock_unlock(&self.pixelBufferLock)

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
            self.consecutiveDecodeErrors += 1
            if self.consecutiveDecodeErrors < self.keyframeErrorThreshold {
                return
            }

            let now = UInt64(VideoReceiver.monotonicNs())
            let elapsed = now - self.lastKeyframeRequestTime
            guard elapsed > self.keyframeRequestCooldownNs else { return }

            self.lastKeyframeRequestTime = now
            self.consecutiveDecodeErrors = 0
            self.controlChannel.requestKeyframe(reason: KeyframeReason.decodeStall.rawValue)
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

        let connectionServer = DiscoveredServer(announce: server.announce, address: serverAddress)
        discovery.sendConnect(to: connectionServer, deviceName: "OXRSys visionOS")
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
        os_unfair_lock_lock(&pixelBufferLock)
        latestPixelBuffer = nil
        os_unfair_lock_unlock(&pixelBufferLock)
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
        os_unfair_lock_lock(&pixelBufferLock)
        let pixelBuffer = latestPixelBuffer
        os_unfair_lock_unlock(&pixelBufferLock)
        return pixelBuffer
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
