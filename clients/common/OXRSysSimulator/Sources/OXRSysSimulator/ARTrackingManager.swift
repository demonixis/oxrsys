// SPDX-License-Identifier: MPL-2.0

#if os(iOS)
// ARTrackingManager.swift — 6DOF head tracking via ARKit world tracking.

import ARKit
import simd

struct TrackingSnapshot: Sendable {
    var position: SIMD3<Float> = .zero
    var orientation: simd_quatf = simd_quatf(ix: 0, iy: 0, iz: 0, r: 1)
    var timestampNs: Int64 = 0
    var isTracking: Bool = false
    var leftHand: DetectedHand? = nil
    var rightHand: DetectedHand? = nil
}

final class ARTrackingManager: NSObject, @unchecked Sendable {
    private let session = ARSession()
    private let queue = DispatchQueue(label: "oxr.arkit", qos: .userInteractive)
    private let handTracker = HandTracker()

    var onTrackingUpdate: (@Sendable (TrackingSnapshot) -> Void)?

    private var running = false
    private nonisolated(unsafe) var positionOffset: SIMD3<Float> = .zero
    private nonisolated(unsafe) var orientationOffset: simd_quatf = simd_quatf(ix: 0, iy: 0, iz: 0, r: 1)
    private nonisolated(unsafe) var lastOutputOrientation: simd_quatf?
    private nonisolated(unsafe) var pendingReset = false

    func resetPose() {
        pendingReset = true
    }

    func start() {
        queue.async { [self] in
            guard !running else { return }

            let config = ARWorldTrackingConfiguration()
            config.worldAlignment = .gravity
            config.isAutoFocusEnabled = false

            session.delegate = self
            session.delegateQueue = queue
            session.run(config)
            running = true
            print("[ARTracking] Started")
        }
    }

    func stop() {
        queue.async { [self] in
            guard running else { return }
            session.pause()
            running = false
            print("[ARTracking] Stopped")
        }
    }
}

extension ARTrackingManager: ARSessionDelegate {
    nonisolated func session(_ session: ARSession, didUpdate frame: ARFrame) {
        let camera = frame.camera
        let transform = camera.transform

        let position = SIMD3<Float>(
            transform.columns.3.x,
            transform.columns.3.y,
            transform.columns.3.z
        )

        let rotationMatrix = simd_float3x3(
            SIMD3<Float>(transform.columns.0.x, transform.columns.0.y, transform.columns.0.z),
            SIMD3<Float>(transform.columns.1.x, transform.columns.1.y, transform.columns.1.z),
            SIMD3<Float>(transform.columns.2.x, transform.columns.2.y, transform.columns.2.z)
        )
        let orientation = simd_quatf(rotationMatrix)
        let isTracking = camera.trackingState == .normal

        if pendingReset {
            pendingReset = false
            positionOffset = position
            orientationOffset = orientation
            lastOutputOrientation = nil
        }

        let adjustedPosition = position - positionOffset
        let adjustedOrientation = stabilized(
            orientationOffset.inverse * orientation,
            previous: lastOutputOrientation
        )
        lastOutputOrientation = adjustedOrientation

        var leftHand: DetectedHand?
        var rightHand: DetectedHand?
        for hand in handTracker.detect(frame: frame) {
            if hand.isLeft {
                leftHand = hand
            } else {
                rightHand = hand
            }
        }

        let snapshot = TrackingSnapshot(
            position: adjustedPosition,
            orientation: adjustedOrientation,
            timestampNs: Int64(frame.timestamp * 1_000_000_000),
            isTracking: isTracking,
            leftHand: leftHand,
            rightHand: rightHand
        )
        onTrackingUpdate?(snapshot)
    }

    private func stabilized(_ orientation: simd_quatf, previous: simd_quatf?) -> simd_quatf {
        let normalized = simd_normalize(orientation)
        guard let previous else { return normalized }
        return simd_dot(previous.vector, normalized.vector) < 0 ? simd_quatf(vector: -normalized.vector) : normalized
    }
}
#endif
