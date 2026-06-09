// SPDX-License-Identifier: MPL-2.0

import CoreGraphics
#if os(macOS)
import AVFoundation
#endif
import OXRSysStreaming
import simd
@testable import OXRSysSimulator
import XCTest

final class WebcamFusionTests: XCTestCase {
    func testSingleCameraKeepsMonoCameraSnapshot() {
        let wrist = SIMD3<Float>(0.1, 1.2, -0.6)
        let frame = sourceFrame(
            cameraID: "primary",
            leftHand: handObservation(cameraID: "primary", isLeft: true, wrist: wrist, quality: 0.8)
        )

        let result = WebcamRigFusion.fuse(
            frames: [frame],
            mode: .singleCamera,
            calibration: nil,
            previous: nil
        )

        XCTAssertEqual(result.activeCameraCount, 1)
        XCTAssertFalse(result.usedCalibratedFusion)
        XCTAssertEqual(result.snapshot?.leftHand?.wristPosition.x ?? 0, wrist.x, accuracy: 0.001)
        XCTAssertEqual(result.snapshot?.leftHand?.wristPosition.y ?? 0, wrist.y, accuracy: 0.001)
        XCTAssertEqual(result.snapshot?.leftHand?.wristPosition.z ?? 0, wrist.z, accuracy: 0.001)
    }

    func testBestViewKeepsVisibleHandWhenOtherCameraIsOccluded() {
        let visible = handObservation(
            cameraID: "secondary",
            isLeft: true,
            wrist: SIMD3<Float>(-0.2, 1.1, -0.5),
            quality: 0.9
        )

        let result = WebcamRigFusion.fuse(
            frames: [
                sourceFrame(cameraID: "primary"),
                sourceFrame(cameraID: "secondary", leftHand: visible),
            ],
            mode: .multiCameraBestView,
            calibration: nil,
            previous: nil
        )

        XCTAssertNotNil(result.snapshot?.leftHand)
        XCTAssertEqual(result.snapshot?.leftHand?.wristPosition.x ?? 0, -0.2, accuracy: 0.001)
    }

    func testCalibratedMultiCameraTriangulatesSyntheticHandPoint() {
        let calibration = rigCalibration()
        let wrist = SIMD3<Float>(0, 0, -1)
        let middle = SIMD3<Float>(0, 0.1, -1)
        let frameA = calibratedFrame(
            cameraID: "leftCam",
            calibration: calibration,
            wrist: wrist,
            middle: middle
        )
        let frameB = calibratedFrame(
            cameraID: "rightCam",
            calibration: calibration,
            wrist: wrist,
            middle: middle
        )

        let result = WebcamRigFusion.fuse(
            frames: [frameA, frameB],
            mode: .calibratedMultiCamera,
            calibration: calibration,
            previous: nil
        )

        XCTAssertTrue(result.usedCalibratedFusion)
        XCTAssertEqual(result.calibratedCameraCount, 2)
        XCTAssertEqual(result.snapshot?.usesCalibratedTrackingSpace, true)
        XCTAssertEqual(result.snapshot?.leftHand?.wristPosition.x ?? 1, wrist.x, accuracy: 0.01)
        XCTAssertEqual(result.snapshot?.leftHand?.wristPosition.y ?? 1, wrist.y, accuracy: 0.01)
        XCTAssertEqual(result.snapshot?.leftHand?.wristPosition.z ?? 1, wrist.z, accuracy: 0.01)
    }

    func testCalibratedMultiCameraRejectsOutlierAndFallsBackToBestView() {
        let calibration = rigCalibration()
        let fallbackWrist = SIMD3<Float>(0.25, 1.2, -0.7)
        let good = handObservation(
            cameraID: "leftCam",
            isLeft: true,
            wrist: fallbackWrist,
            quality: 0.9,
            normalizedJoints: Array(
                repeating: SIMD2<Float>(0.5, 0.5),
                count: OXRProtocol.handJointCount
            )
        )
        let outlier = handObservation(
            cameraID: "rightCam",
            isLeft: true,
            wrist: SIMD3<Float>(-0.4, 0.4, -0.4),
            quality: 0.6,
            normalizedJoints: Array(
                repeating: SIMD2<Float>(0.95, 0.95),
                count: OXRProtocol.handJointCount
            )
        )

        let result = WebcamRigFusion.fuse(
            frames: [
                sourceFrame(cameraID: "leftCam", leftHand: good),
                sourceFrame(cameraID: "rightCam", leftHand: outlier),
            ],
            mode: .calibratedMultiCamera,
            calibration: calibration,
            previous: nil
        )

        XCTAssertFalse(result.usedCalibratedFusion)
        XCTAssertEqual(result.snapshot?.usesCalibratedTrackingSpace, false)
        XCTAssertEqual(result.snapshot?.leftHand?.wristPosition.x ?? 0, fallbackWrist.x, accuracy: 0.001)
        XCTAssertEqual(result.snapshot?.leftHand?.wristPosition.y ?? 0, fallbackWrist.y, accuracy: 0.001)
        XCTAssertEqual(result.snapshot?.leftHand?.wristPosition.z ?? 0, fallbackWrist.z, accuracy: 0.001)
    }

    func testBestViewKeepsLeftAndRightHandsSeparatedAcrossCameras() {
        let left = handObservation(
            cameraID: "primary",
            isLeft: true,
            wrist: SIMD3<Float>(-0.2, 1.1, -0.6),
            quality: 0.8
        )
        let right = handObservation(
            cameraID: "secondary",
            isLeft: false,
            wrist: SIMD3<Float>(0.2, 1.1, -0.6),
            quality: 0.8
        )

        let result = WebcamRigFusion.fuse(
            frames: [
                sourceFrame(cameraID: "primary", leftHand: left),
                sourceFrame(cameraID: "secondary", rightHand: right),
            ],
            mode: .multiCameraBestView,
            calibration: nil,
            previous: nil
        )

        XCTAssertEqual(result.snapshot?.leftHand?.isLeft, true)
        XCTAssertEqual(result.snapshot?.rightHand?.isLeft, false)
        XCTAssertEqual(result.snapshot?.leftHand?.wristPosition.x ?? 0, -0.2, accuracy: 0.001)
        XCTAssertEqual(result.snapshot?.rightHand?.wristPosition.x ?? 0, 0.2, accuracy: 0.001)
    }

    func testPreviewPointsIncludeHeadAndHandLandmarks() {
        var normalizedJoints = Array<SIMD2<Float>?>(repeating: nil, count: OXRProtocol.handJointCount)
        normalizedJoints[1] = SIMD2<Float>(0.4, 0.5)
        normalizedJoints[10] = SIMD2<Float>(0.45, 0.7)
        let head = WebcamHeadObservation(
            cameraID: "primary",
            head: WebcamHeadPose(position: SIMD3<Float>(0, 1.6, -0.4), orientation: simd_quatf()),
            normalizedCenter: SIMD2<Float>(0.5, 0.8),
            quality: 0.9
        )
        let left = handObservation(
            cameraID: "primary",
            isLeft: true,
            wrist: SIMD3<Float>(-0.2, 1.1, -0.6),
            quality: 0.8,
            normalizedJoints: normalizedJoints
        )

        let points = WebcamPreviewFrame.trackingPoints(
            head: head,
            leftHand: left,
            rightHand: nil
        )

        XCTAssertEqual(points.count, 3)
        XCTAssertEqual(points.filter { $0.kind == .head }.count, 1)
        XCTAssertEqual(points.filter { $0.kind == .leftHand }.count, 2)
        XCTAssertEqual(points.filter { $0.kind == .rightHand }.count, 0)
    }

    func testPreviewPointPixelPositionUsesRawCameraXCoordinate() {
        let point = WebcamPreviewPoint(
            normalizedPosition: SIMD2<Float>(0.25, 0.75),
            kind: .leftHand,
            confidence: 1
        )
        let drawRect = CGRect(x: 10, y: 20, width: 400, height: 200)

        let position = point.pixelPosition(in: drawRect)

        XCTAssertEqual(position.x, 110, accuracy: 0.001)
        XCTAssertEqual(position.y, 70, accuracy: 0.001)
        XCTAssertLessThan(position.x, drawRect.midX)
    }

    #if os(macOS)
    @MainActor
    func testPreviewClosePreventsLateFramesFromCreatingWindows() {
        let manager = WebcamPreviewWindowManager()
        let device = DesktopWebcamDevice(id: "camera", name: "Camera")
        manager.showWindows(for: [device])
        XCTAssertTrue(manager.isOpen)

        manager.closeAll()
        XCTAssertFalse(manager.isOpen)

        manager.show(frame: previewFrame(cameraID: device.id, timestampNs: 1))
        XCTAssertFalse(manager.isOpen)
        XCTAssertNil(manager.frame(for: device.id))
    }

    @MainActor
    func testPreviewCanReopenAndAcceptNewFrames() {
        let manager = WebcamPreviewWindowManager()
        let device = DesktopWebcamDevice(id: "camera", name: "Camera")
        manager.showWindows(for: [device])
        manager.closeAll()

        manager.showWindows(for: [device])
        let frame = previewFrame(cameraID: device.id, timestampNs: 2)
        manager.show(frame: frame)

        XCTAssertTrue(manager.isOpen)
        XCTAssertEqual(manager.frame(for: device.id)?.timestampNs, 2)
        manager.closeAll()
    }

    func testDesktopWebcamConfigurationDefaultsToMediumCaptureResolution() {
        let configuration = DesktopWebcamTrackingConfiguration(
            sourceMode: .singleCamera,
            primaryDeviceID: "",
            secondaryDeviceID: "",
            calibration: nil
        )

        XCTAssertEqual(configuration.captureResolution, .medium)
    }

    func testCaptureResolutionFallbackOrderPrefersRequestedPreset() {
        XCTAssertEqual(WebcamCaptureResolution.hd1080.fallbackPresets.first, .hd1920x1080)
        XCTAssertTrue(WebcamCaptureResolution.hd1080.fallbackPresets.contains(.hd1280x720))
        XCTAssertTrue(WebcamCaptureResolution.hd1080.fallbackPresets.contains(.medium))
        XCTAssertEqual(WebcamCaptureResolution.low.fallbackPresets.first, .low)
    }

    func testDepthEstimatorReturnsLargerDepthForSmallerImageHand() {
        let intrinsics = WebcamCameraIntrinsics.estimated(width: 640, height: 480)
        let near = WebcamHandDepthEstimator.estimateDepth(
            landmarks: depthLandmarks(middleY: 0.68),
            intrinsics: intrinsics,
            settings: WebcamTrackingSettings()
        )
        let far = WebcamHandDepthEstimator.estimateDepth(
            landmarks: depthLandmarks(middleY: 0.58),
            intrinsics: intrinsics,
            settings: WebcamTrackingSettings()
        )

        XCTAssertNotNil(near)
        XCTAssertNotNil(far)
        XCTAssertGreaterThan(far ?? 0, near ?? 0)
    }

    func testWeightedDepthMedianIgnoresLowWeightOutlier() {
        let depth = WebcamHandDepthEstimator.weightedMedian(
            samples: [
                WebcamHandDepthSample(depth: 0.8, weight: 1),
                WebcamHandDepthSample(depth: 0.82, weight: 1),
                WebcamHandDepthSample(depth: 2.0, weight: 0.01),
            ]
        )

        XCTAssertEqual(depth ?? 0, 0.82, accuracy: 0.001)
    }

    func testDepthSmoothingLimitsLargeJumps() {
        var settings = WebcamTrackingSettings()
        settings.handDepthSmoothing = 0.5

        let smoothed = WebcamHandDepthEstimator.smoothDepth(
            previous: 0.6,
            raw: 0.8,
            settings: settings
        )
        let rejected = WebcamHandDepthEstimator.smoothDepth(
            previous: 0.6,
            raw: 1.5,
            settings: settings
        )

        XCTAssertEqual(smoothed, 0.7, accuracy: 0.001)
        XCTAssertEqual(rejected, 0.6, accuracy: 0.001)
    }
    #endif

    #if os(macOS)
    private func previewFrame(cameraID: String, timestampNs: Int64) -> WebcamPreviewFrame {
        WebcamPreviewFrame(
            cameraID: cameraID,
            cameraName: "Camera",
            previewGeneration: 1,
            timestampNs: timestampNs,
            image: nil,
            imageWidth: 640,
            imageHeight: 480,
            points: [],
            debugText: nil
        )
    }

    private func depthLandmarks(middleY: Float) -> [WebcamHandDepthJoint: WebcamHandDepthLandmark] {
        let delta = max(middleY - 0.5, 0.03)
        return [
            .wrist: WebcamHandDepthLandmark(
                normalizedPosition: SIMD2<Float>(0.5, 0.5),
                confidence: 1
            ),
            .middleMCP: WebcamHandDepthLandmark(
                normalizedPosition: SIMD2<Float>(0.5, middleY),
                confidence: 1
            ),
            .indexMCP: WebcamHandDepthLandmark(
                normalizedPosition: SIMD2<Float>(0.5 - delta * 0.45, 0.5 + delta * 0.55),
                confidence: 1
            ),
            .littleMCP: WebcamHandDepthLandmark(
                normalizedPosition: SIMD2<Float>(0.5 + delta * 0.45, 0.5 + delta * 0.55),
                confidence: 1
            ),
        ]
    }
    #endif

    private func sourceFrame(
        cameraID: String,
        leftHand: WebcamHandObservation? = nil,
        rightHand: WebcamHandObservation? = nil
    ) -> WebcamSourceFrame {
        WebcamSourceFrame(
            cameraID: cameraID,
            cameraName: cameraID,
            timestampNs: 1,
            intrinsics: WebcamCameraIntrinsics.estimated(width: 100, height: 100),
            head: nil,
            leftHand: leftHand,
            rightHand: rightHand
        )
    }

    private func handObservation(
        cameraID: String,
        isLeft: Bool,
        wrist: SIMD3<Float>,
        quality: Float,
        normalizedJoints: [SIMD2<Float>?]? = nil
    ) -> WebcamHandObservation {
        var joints = Array(repeating: wrist, count: OXRProtocol.handJointCount)
        joints[0] = wrist + SIMD3<Float>(0, 0.04, 0)
        joints[1] = wrist
        joints[12] = wrist + SIMD3<Float>(0, 0.1, 0)
        return WebcamHandObservation(
            cameraID: cameraID,
            hand: WebcamTrackedHand(isLeft: isLeft, joints: joints, wristPosition: wrist),
            normalizedJoints: normalizedJoints ??
                Array(repeating: SIMD2<Float>(0.5, 0.5), count: OXRProtocol.handJointCount),
            quality: quality
        )
    }

    private func calibratedFrame(
        cameraID: String,
        calibration: WebcamRigCalibration,
        wrist: SIMD3<Float>,
        middle: SIMD3<Float>
    ) -> WebcamSourceFrame {
        let camera = calibration.camera(for: cameraID)!
        var normalized = Array(
            repeating: project(wrist, camera: camera),
            count: OXRProtocol.handJointCount
        )
        normalized[12] = project(middle, camera: camera)
        let fallbackWrist = SIMD3<Float>(0.4, 0.4, -0.4)
        return sourceFrame(
            cameraID: cameraID,
            leftHand: handObservation(
                cameraID: cameraID,
                isLeft: true,
                wrist: fallbackWrist,
                quality: 0.8,
                normalizedJoints: normalized
            )
        )
    }

    private func rigCalibration() -> WebcamRigCalibration {
        let intrinsics = WebcamCameraIntrinsics(
            width: 100,
            height: 100,
            fx: 100,
            fy: 100,
            cx: 50,
            cy: 50
        )
        return WebcamRigCalibration(
            cameras: [
                WebcamCameraCalibration(
                    deviceID: "leftCam",
                    intrinsics: intrinsics,
                    cameraToTrackingTranslation: SIMD3<Float>(-0.1, 0, 0)
                ),
                WebcamCameraCalibration(
                    deviceID: "rightCam",
                    intrinsics: intrinsics,
                    cameraToTrackingTranslation: SIMD3<Float>(0.1, 0, 0)
                ),
            ],
            reprojectionError: 0.01
        )
    }

    private func project(
        _ point: SIMD3<Float>,
        camera: WebcamCameraCalibration
    ) -> SIMD2<Float> {
        let translation = camera.translation!
        let cameraPoint = point - translation
        let xPixel = camera.cx + camera.fx * (cameraPoint.x / -cameraPoint.z)
        let yPixel = camera.cy - camera.fy * (cameraPoint.y / -cameraPoint.z)
        return SIMD2<Float>(
            xPixel / camera.imageWidth,
            1 - yPixel / camera.imageHeight
        )
    }
}
