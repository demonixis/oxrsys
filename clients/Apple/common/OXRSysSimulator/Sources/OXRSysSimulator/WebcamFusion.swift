// SPDX-License-Identifier: MPL-2.0

import Foundation
import CoreGraphics
import OXRSysStreaming
import simd

enum WebcamTrackingSourceMode: String, CaseIterable, Identifiable, Sendable {
    case singleCamera = "Single Camera"
    case multiCameraBestView = "Multi-Camera Best View"
    case calibratedMultiCamera = "Calibrated Multi-Camera"

    var id: String { rawValue }
}

struct WebcamCameraIntrinsics: Sendable {
    var width: Float
    var height: Float
    var fx: Float
    var fy: Float
    var cx: Float
    var cy: Float

    static func estimated(width: Int, height: Int) -> WebcamCameraIntrinsics {
        let w = max(Float(width), 1)
        let h = max(Float(height), 1)
        let assumedHorizontalFOV = Float(60.0 * .pi / 180.0)
        let focal = w / (2 * tan(assumedHorizontalFOV / 2))
        return WebcamCameraIntrinsics(
            width: w,
            height: h,
            fx: focal,
            fy: focal,
            cx: w * 0.5,
            cy: h * 0.5
        )
    }
}

struct WebcamRigCalibration: Codable, Sendable {
    var cameras: [WebcamCameraCalibration]
    var reprojectionError: Float?

    var isUsable: Bool {
        cameras.count >= 2
    }

    func camera(for deviceID: String) -> WebcamCameraCalibration? {
        cameras.first { $0.deviceID == deviceID }
    }
}

struct WebcamCameraCalibration: Codable, Sendable {
    var deviceID: String
    var imageWidth: Float
    var imageHeight: Float
    var fx: Float
    var fy: Float
    var cx: Float
    var cy: Float
    var cameraToTrackingRotation: [Float]
    var cameraToTrackingTranslation: [Float]

    init(
        deviceID: String,
        imageWidth: Float,
        imageHeight: Float,
        fx: Float,
        fy: Float,
        cx: Float,
        cy: Float,
        cameraToTrackingRotation: [Float],
        cameraToTrackingTranslation: [Float]
    ) {
        self.deviceID = deviceID
        self.imageWidth = imageWidth
        self.imageHeight = imageHeight
        self.fx = fx
        self.fy = fy
        self.cx = cx
        self.cy = cy
        self.cameraToTrackingRotation = cameraToTrackingRotation
        self.cameraToTrackingTranslation = cameraToTrackingTranslation
    }

    init(
        deviceID: String,
        intrinsics: WebcamCameraIntrinsics,
        cameraToTrackingRotation: simd_float3x3 = matrix_identity_float3x3,
        cameraToTrackingTranslation: SIMD3<Float> = .zero
    ) {
        let columns = cameraToTrackingRotation.columns
        self.init(
            deviceID: deviceID,
            imageWidth: intrinsics.width,
            imageHeight: intrinsics.height,
            fx: intrinsics.fx,
            fy: intrinsics.fy,
            cx: intrinsics.cx,
            cy: intrinsics.cy,
            cameraToTrackingRotation: [
                columns.0.x,
                columns.1.x,
                columns.2.x,
                columns.0.y,
                columns.1.y,
                columns.2.y,
                columns.0.z,
                columns.1.z,
                columns.2.z,
            ],
            cameraToTrackingTranslation: [
                cameraToTrackingTranslation.x,
                cameraToTrackingTranslation.y,
                cameraToTrackingTranslation.z,
            ]
        )
    }

    var intrinsics: WebcamCameraIntrinsics {
        WebcamCameraIntrinsics(
            width: imageWidth,
            height: imageHeight,
            fx: fx,
            fy: fy,
            cx: cx,
            cy: cy
        )
    }

    var rotationMatrix: simd_float3x3? {
        guard cameraToTrackingRotation.count == 9 else { return nil }
        return simd_float3x3(
            columns: (
                SIMD3<Float>(
                    cameraToTrackingRotation[0],
                    cameraToTrackingRotation[3],
                    cameraToTrackingRotation[6]
                ),
                SIMD3<Float>(
                    cameraToTrackingRotation[1],
                    cameraToTrackingRotation[4],
                    cameraToTrackingRotation[7]
                ),
                SIMD3<Float>(
                    cameraToTrackingRotation[2],
                    cameraToTrackingRotation[5],
                    cameraToTrackingRotation[8]
                )
            )
        )
    }

    var translation: SIMD3<Float>? {
        guard cameraToTrackingTranslation.count == 3 else { return nil }
        return SIMD3<Float>(
            cameraToTrackingTranslation[0],
            cameraToTrackingTranslation[1],
            cameraToTrackingTranslation[2]
        )
    }
}

struct WebcamSourceFrame: Sendable {
    var cameraID: String
    var cameraName: String
    var timestampNs: Int64
    var intrinsics: WebcamCameraIntrinsics?
    var head: WebcamHeadObservation?
    var leftHand: WebcamHandObservation?
    var rightHand: WebcamHandObservation?

    var isTracking: Bool {
        head != nil || leftHand != nil || rightHand != nil
    }
}

struct WebcamHeadObservation: Sendable {
    var cameraID: String
    var head: WebcamHeadPose
    var normalizedCenter: SIMD2<Float>
    var quality: Float
}

struct WebcamHandObservation: Sendable {
    var cameraID: String
    var hand: WebcamTrackedHand
    var normalizedJoints: [SIMD2<Float>?]
    var quality: Float
}

struct WebcamFusionResult: Sendable {
    var snapshot: WebcamTrackingSnapshot?
    var activeCameraCount: Int
    var calibratedCameraCount: Int
    var usedCalibratedFusion: Bool
    var confidence: Float
}

enum WebcamPreviewPointKind: Sendable, Equatable {
    case head
    case leftHand
    case rightHand
}

struct WebcamPreviewPoint: Sendable {
    var normalizedPosition: SIMD2<Float>
    var kind: WebcamPreviewPointKind
    var confidence: Float

    func pixelPosition(in drawRect: CGRect) -> CGPoint {
        CGPoint(
            x: drawRect.minX + CGFloat(normalizedPosition.x) * drawRect.width,
            y: drawRect.minY + CGFloat(1 - normalizedPosition.y) * drawRect.height
        )
    }
}

struct WebcamPreviewFrame: @unchecked Sendable {
    var cameraID: String
    var cameraName: String
    var previewGeneration: UInt64 = 0
    var timestampNs: Int64
    var image: CGImage?
    var imageWidth: Int
    var imageHeight: Int
    var points: [WebcamPreviewPoint]
    var debugText: String? = nil

    static func trackingPoints(
        head: WebcamHeadObservation?,
        leftHand: WebcamHandObservation?,
        rightHand: WebcamHandObservation?
    ) -> [WebcamPreviewPoint] {
        var points: [WebcamPreviewPoint] = []

        if let head {
            points.append(
                WebcamPreviewPoint(
                    normalizedPosition: head.normalizedCenter,
                    kind: .head,
                    confidence: head.quality
                )
            )
        }

        appendHandPoints(leftHand, kind: .leftHand, to: &points)
        appendHandPoints(rightHand, kind: .rightHand, to: &points)
        return points
    }

    static func depthDebugText(
        leftHand: WebcamHandObservation?,
        rightHand: WebcamHandObservation?
    ) -> String? {
        var parts: [String] = []
        if let depth = leftHand?.hand.estimatedDepth {
            parts.append(String(format: "L %.2fm", depth))
        }
        if let depth = rightHand?.hand.estimatedDepth {
            parts.append(String(format: "R %.2fm", depth))
        }
        return parts.isEmpty ? nil : parts.joined(separator: "  ")
    }

    private static func appendHandPoints(
        _ hand: WebcamHandObservation?,
        kind: WebcamPreviewPointKind,
        to points: inout [WebcamPreviewPoint]
    ) {
        guard let hand else { return }
        for point in hand.normalizedJoints.compactMap({ $0 }) {
            points.append(
                WebcamPreviewPoint(
                    normalizedPosition: point,
                    kind: kind,
                    confidence: hand.quality
                )
            )
        }
    }
}

enum WebcamRigFusion {
    private static let maxFrameAgeNs: Int64 = 250_000_000
    private static let triangulationErrorLimit: Float = 0.18

    static func fuse(
        frames: [WebcamSourceFrame],
        mode: WebcamTrackingSourceMode,
        calibration: WebcamRigCalibration?,
        previous: WebcamTrackingSnapshot?
    ) -> WebcamFusionResult {
        guard !frames.isEmpty else {
            return WebcamFusionResult(
                snapshot: nil,
                activeCameraCount: 0,
                calibratedCameraCount: 0,
                usedCalibratedFusion: false,
                confidence: 0
            )
        }

        let newestTimestamp = frames.map(\.timestampNs).max() ?? 0
        let freshFrames = frames.filter { newestTimestamp - $0.timestampNs <= maxFrameAgeNs }
        let usableFrames = freshFrames.isEmpty ? frames : freshFrames
        let calibratedCameraCount = usableFrames.filter { frame in
            calibration?.camera(for: frame.cameraID) != nil
        }.count

        switch mode {
        case .singleCamera, .multiCameraBestView:
            return bestViewResult(
                frames: mode == .singleCamera ? Array(usableFrames.prefix(1)) : usableFrames,
                newestTimestamp: newestTimestamp,
                calibratedCameraCount: calibratedCameraCount,
                previous: previous
            )
        case .calibratedMultiCamera:
            guard let calibration, calibration.isUsable, calibratedCameraCount >= 2 else {
                return bestViewResult(
                    frames: usableFrames,
                    newestTimestamp: newestTimestamp,
                    calibratedCameraCount: calibratedCameraCount,
                    previous: previous
                )
            }
            return calibratedResult(
                frames: usableFrames,
                newestTimestamp: newestTimestamp,
                calibration: calibration,
                calibratedCameraCount: calibratedCameraCount,
                previous: previous
            )
        }
    }

    private static func bestViewResult(
        frames: [WebcamSourceFrame],
        newestTimestamp: Int64,
        calibratedCameraCount: Int,
        previous: WebcamTrackingSnapshot?
    ) -> WebcamFusionResult {
        let head = bestHead(frames: frames, newestTimestamp: newestTimestamp, previous: previous?.head)
        let leftHand = bestHand(
            frames: frames,
            newestTimestamp: newestTimestamp,
            side: .left,
            previous: previous?.leftHand
        )
        let rightHand = bestHand(
            frames: frames,
            newestTimestamp: newestTimestamp,
            side: .right,
            previous: previous?.rightHand
        )
        let confidence = max(head?.quality ?? 0, leftHand?.quality ?? 0, rightHand?.quality ?? 0)
        let snapshot = WebcamTrackingSnapshot(
            timestampNs: newestTimestamp,
            head: head?.head,
            leftHand: leftHand?.hand,
            rightHand: rightHand?.hand,
            usesCalibratedTrackingSpace: false
        )
        return WebcamFusionResult(
            snapshot: snapshot.isTracking ? snapshot : nil,
            activeCameraCount: frames.count,
            calibratedCameraCount: calibratedCameraCount,
            usedCalibratedFusion: false,
            confidence: confidence
        )
    }

    private static func calibratedResult(
        frames: [WebcamSourceFrame],
        newestTimestamp: Int64,
        calibration: WebcamRigCalibration,
        calibratedCameraCount: Int,
        previous: WebcamTrackingSnapshot?
    ) -> WebcamFusionResult {
        let fallbackHead = bestHead(frames: frames, newestTimestamp: newestTimestamp, previous: previous?.head)
        let fallbackLeft = bestHand(
            frames: frames,
            newestTimestamp: newestTimestamp,
            side: .left,
            previous: previous?.leftHand
        )
        let fallbackRight = bestHand(
            frames: frames,
            newestTimestamp: newestTimestamp,
            side: .right,
            previous: previous?.rightHand
        )

        var usedCalibratedFusion = false
        let head = calibratedHead(
            frames: frames,
            calibration: calibration,
            fallback: fallbackHead
        ) { usedCalibratedFusion = true }
        let leftHand = calibratedHand(
            frames: frames,
            side: .left,
            calibration: calibration,
            fallback: fallbackLeft
        ) { usedCalibratedFusion = true }
        let rightHand = calibratedHand(
            frames: frames,
            side: .right,
            calibration: calibration,
            fallback: fallbackRight
        ) { usedCalibratedFusion = true }

        let confidence = max(head?.quality ?? 0, leftHand?.quality ?? 0, rightHand?.quality ?? 0)
        let snapshot = WebcamTrackingSnapshot(
            timestampNs: newestTimestamp,
            head: head?.head,
            leftHand: leftHand?.hand,
            rightHand: rightHand?.hand,
            usesCalibratedTrackingSpace: usedCalibratedFusion
        )
        return WebcamFusionResult(
            snapshot: snapshot.isTracking ? snapshot : nil,
            activeCameraCount: frames.count,
            calibratedCameraCount: calibratedCameraCount,
            usedCalibratedFusion: usedCalibratedFusion,
            confidence: confidence
        )
    }

    private static func calibratedHead(
        frames: [WebcamSourceFrame],
        calibration: WebcamRigCalibration,
        fallback: WebcamHeadObservation?,
        didUseCalibration: () -> Void
    ) -> WebcamHeadObservation? {
        let rays = frames.compactMap { frame -> CalibrationRaySample? in
            guard let observation = frame.head,
                  let camera = calibration.camera(for: frame.cameraID),
                  let ray = raySample(
                    camera: camera,
                    normalizedPoint: observation.normalizedCenter,
                    quality: observation.quality
                  ) else {
                return nil
            }
            return ray
        }

        guard let triangulated = triangulate(rays: rays),
              triangulated.error <= triangulationErrorLimit,
              var result = fallback else {
            return fallback
        }

        result.head.position = triangulated.position
        result.quality = min(1, max(result.quality, 1 - triangulated.error / triangulationErrorLimit))
        didUseCalibration()
        return result
    }

    private static func calibratedHand(
        frames: [WebcamSourceFrame],
        side: HandSide,
        calibration: WebcamRigCalibration,
        fallback: WebcamHandObservation?,
        didUseCalibration: () -> Void
    ) -> WebcamHandObservation? {
        let observations = frames.compactMap { frame -> WebcamHandObservation? in
            switch side {
            case .left:
                return frame.leftHand
            case .right:
                return frame.rightHand
            }
        }
        guard observations.count >= 2, let fallback else { return fallback }

        var joints = fallback.hand.joints
        var triangulatedCount = 0
        var accumulatedError: Float = 0

        for index in 0..<min(OXRProtocol.handJointCount, joints.count) {
            let rays = observations.compactMap { observation -> CalibrationRaySample? in
                guard observation.normalizedJoints.indices.contains(index),
                      let normalizedPoint = observation.normalizedJoints[index],
                      let camera = calibration.camera(for: observation.cameraID) else {
                    return nil
                }
                return raySample(
                    camera: camera,
                    normalizedPoint: normalizedPoint,
                    quality: observation.quality
                )
            }

            guard let triangulated = triangulate(rays: rays),
                  triangulated.error <= triangulationErrorLimit else {
                continue
            }

            joints[index] = triangulated.position
            triangulatedCount += 1
            accumulatedError += triangulated.error
        }

        guard triangulatedCount >= 2 else { return fallback }

        let averageError = accumulatedError / Float(triangulatedCount)
        didUseCalibration()
        return WebcamHandObservation(
            cameraID: fallback.cameraID,
            hand: WebcamTrackedHand(
                isLeft: fallback.hand.isLeft,
                joints: joints,
                wristPosition: joints.indices.contains(1) ? joints[1] : fallback.hand.wristPosition,
                estimatedDepth: fallback.hand.estimatedDepth
            ),
            normalizedJoints: fallback.normalizedJoints,
            quality: min(1, max(fallback.quality, 1 - averageError / triangulationErrorLimit))
        )
    }

    private static func bestHead(
        frames: [WebcamSourceFrame],
        newestTimestamp: Int64,
        previous: WebcamHeadPose?
    ) -> WebcamHeadObservation? {
        frames.compactMap(\.head).max { lhs, rhs in
            headScore(lhs, newestTimestamp: newestTimestamp, previous: previous) <
                headScore(rhs, newestTimestamp: newestTimestamp, previous: previous)
        }
    }

    private static func bestHand(
        frames: [WebcamSourceFrame],
        newestTimestamp: Int64,
        side: HandSide,
        previous: WebcamTrackedHand?
    ) -> WebcamHandObservation? {
        frames.compactMap { frame -> WebcamHandObservation? in
            switch side {
            case .left:
                return frame.leftHand
            case .right:
                return frame.rightHand
            }
        }.max { lhs, rhs in
            handScore(lhs, newestTimestamp: newestTimestamp, previous: previous) <
                handScore(rhs, newestTimestamp: newestTimestamp, previous: previous)
        }
    }

    private static func headScore(
        _ observation: WebcamHeadObservation,
        newestTimestamp: Int64,
        previous: WebcamHeadPose?
    ) -> Float {
        var score = observation.quality
        if let previous {
            let distance = simd_length(observation.head.position - previous.position)
            score -= min(distance * 0.25, 0.35)
        }
        return score
    }

    private static func handScore(
        _ observation: WebcamHandObservation,
        newestTimestamp: Int64,
        previous: WebcamTrackedHand?
    ) -> Float {
        var score = observation.quality
        if let previous {
            let distance = simd_length(observation.hand.wristPosition - previous.wristPosition)
            score -= min(distance * 0.35, 0.45)
        }
        return score
    }

    private static func raySample(
        camera: WebcamCameraCalibration,
        normalizedPoint: SIMD2<Float>,
        quality: Float
    ) -> CalibrationRaySample? {
        guard let rotation = camera.rotationMatrix,
              let translation = camera.translation else {
            return nil
        }

        let xPixel = normalizedPoint.x * camera.imageWidth
        let yPixel = (1 - normalizedPoint.y) * camera.imageHeight
        let xCamera = (xPixel - camera.cx) / camera.fx
        let yCamera = -(yPixel - camera.cy) / camera.fy
        let cameraDirection = simd_normalize(SIMD3<Float>(xCamera, yCamera, -1))
        return CalibrationRaySample(
            origin: translation,
            direction: simd_normalize(rotation * cameraDirection),
            quality: quality
        )
    }

    private static func triangulate(rays: [CalibrationRaySample]) -> (position: SIMD3<Float>, error: Float)? {
        guard rays.count >= 2 else { return nil }

        var weightedPoint = SIMD3<Float>(0, 0, 0)
        var totalWeight: Float = 0
        var totalError: Float = 0
        var pairCount: Float = 0

        for lhsIndex in 0..<(rays.count - 1) {
            for rhsIndex in (lhsIndex + 1)..<rays.count {
                guard let pair = closestPointBetween(
                    first: rays[lhsIndex],
                    second: rays[rhsIndex]
                ) else {
                    continue
                }
                let weight = max(0.01, rays[lhsIndex].quality * rays[rhsIndex].quality)
                weightedPoint += pair.position * weight
                totalWeight += weight
                totalError += pair.error
                pairCount += 1
            }
        }

        guard totalWeight > 0, pairCount > 0 else { return nil }
        return (weightedPoint / totalWeight, totalError / pairCount)
    }

    private static func closestPointBetween(
        first: CalibrationRaySample,
        second: CalibrationRaySample
    ) -> (position: SIMD3<Float>, error: Float)? {
        let p1 = first.origin
        let p2 = second.origin
        let d1 = simd_normalize(first.direction)
        let d2 = simd_normalize(second.direction)
        let r = p1 - p2
        let a = simd_dot(d1, d1)
        let e = simd_dot(d2, d2)
        let b = simd_dot(d1, d2)
        let c = simd_dot(d1, r)
        let f = simd_dot(d2, r)
        let denominator = a * e - b * b

        guard abs(denominator) > 0.000001 else { return nil }

        let s = (b * f - c * e) / denominator
        let t = (a * f - b * c) / denominator
        guard s >= 0, t >= 0 else { return nil }

        let closestFirst = p1 + d1 * s
        let closestSecond = p2 + d2 * t
        return ((closestFirst + closestSecond) * 0.5, simd_length(closestFirst - closestSecond))
    }

    private enum HandSide {
        case left
        case right
    }

    private struct CalibrationRaySample {
        var origin: SIMD3<Float>
        var direction: SIMD3<Float>
        var quality: Float
    }
}
