// SPDX-License-Identifier: MPL-2.0

import OXRSysStreaming
import simd
@testable import OXRSysSimulator
import XCTest

final class WebcamGestureMapperTests: XCTestCase {
    func testPinchIndexActivatesTriggerInControllerMode() {
        let hand = makeHand(
            isLeft: true,
            indexPinched: true,
            middleCurl: .open,
            ringCurl: .open,
            pinkyCurl: .open
        )
        let packet = WebcamGestureMapper.makeTrackingPacket(
            basePacket: basePacket(),
            snapshot: snapshot(leftHand: hand),
            outputMode: .controllersFromHands,
            headTrackingEnabled: false
        )

        XCTAssertNotEqual(packet.trackingFlags & TrackingFlagsValues.leftControllerActive, 0)
        XCTAssertEqual(packet.trackingFlags & TrackingFlagsValues.leftHandActive, 0)
        XCTAssertGreaterThan(packet.leftTrigger, WebcamGestureMapper.triggerClickThreshold)
        XCTAssertNotEqual(packet.buttonState & ButtonFlags.leftTrigger, 0)
    }

    func testMiddleAndRingCurlActivateGripWithNoisyPinky() {
        let hand = makeHand(
            isLeft: false,
            indexPinched: false,
            middleCurl: .closed,
            ringCurl: .closed,
            pinkyCurl: .open
        )
        let values = WebcamGestureMapper.gestureValues(for: hand)

        XCTAssertGreaterThan(values.grip, WebcamGestureMapper.gripClickThreshold)

        let packet = WebcamGestureMapper.makeTrackingPacket(
            basePacket: basePacket(),
            snapshot: snapshot(rightHand: hand),
            outputMode: .controllersFromHands,
            headTrackingEnabled: false
        )

        XCTAssertNotEqual(packet.trackingFlags & TrackingFlagsValues.rightControllerActive, 0)
        XCTAssertEqual(packet.trackingFlags & TrackingFlagsValues.rightHandActive, 0)
        XCTAssertGreaterThan(packet.rightGrip, WebcamGestureMapper.gripClickThreshold)
        XCTAssertNotEqual(packet.buttonState & ButtonFlags.rightGrip, 0)
    }

    func testHandAndControllerOutputModesUseMutuallyExclusiveFlags() {
        let hand = makeHand(
            isLeft: true,
            indexPinched: true,
            middleCurl: .closed,
            ringCurl: .closed,
            pinkyCurl: .closed
        )
        let handPacket = WebcamGestureMapper.makeTrackingPacket(
            basePacket: basePacket(),
            snapshot: snapshot(leftHand: hand),
            outputMode: .hands,
            headTrackingEnabled: false
        )
        let controllerPacket = WebcamGestureMapper.makeTrackingPacket(
            basePacket: basePacket(),
            snapshot: snapshot(leftHand: hand),
            outputMode: .controllersFromHands,
            headTrackingEnabled: false
        )

        XCTAssertNotEqual(handPacket.trackingFlags & TrackingFlagsValues.leftHandActive, 0)
        XCTAssertEqual(handPacket.trackingFlags & TrackingFlagsValues.leftControllerActive, 0)
        XCTAssertEqual(handPacket.leftTrigger, 0)
        XCTAssertEqual(handPacket.buttonState & ButtonFlags.leftTrigger, 0)

        XCTAssertEqual(controllerPacket.trackingFlags & TrackingFlagsValues.leftHandActive, 0)
        XCTAssertNotEqual(controllerPacket.trackingFlags & TrackingFlagsValues.leftControllerActive, 0)
        XCTAssertGreaterThan(controllerPacket.leftTrigger, WebcamGestureMapper.triggerClickThreshold)
    }

    func testTrackingSpaceConvertsCameraHeightForLocalSpace() {
        var settings = WebcamTrackingSettings()
        settings.cameraY = 1.6
        settings.trackingSpace = .localFloor
        XCTAssertEqual(settings.yInTrackingSpace(1.6), 1.6, accuracy: 0.001)

        settings.trackingSpace = .local
        XCTAssertEqual(settings.yInTrackingSpace(1.6), 0.0, accuracy: 0.001)
    }

    func testUserFacingHandMappingMirrorsImageXIntoVRSpace() {
        let mapper = WebcamCameraToTrackingMapper(settings: WebcamTrackingSettings())

        let left = mapper.handPoint(normalizedPoint: SIMD2<Float>(0.25, 0.5), depth: 0.8)
        let right = mapper.handPoint(normalizedPoint: SIMD2<Float>(0.75, 0.5), depth: 0.8)

        XCTAssertGreaterThan(left.x, 0)
        XCTAssertLessThan(right.x, 0)
        XCTAssertEqual(left.y, right.y, accuracy: 0.001)
        XCTAssertEqual(left.z, right.z, accuracy: 0.001)
    }

    func testRawCameraHandMappingKeepsImageXInCameraSpace() {
        var settings = WebcamTrackingSettings()
        settings.cameraFacing = .rawCamera
        let mapper = WebcamCameraToTrackingMapper(settings: settings)

        let left = mapper.handPoint(normalizedPoint: SIMD2<Float>(0.25, 0.5), depth: 0.8)
        let right = mapper.handPoint(normalizedPoint: SIMD2<Float>(0.75, 0.5), depth: 0.8)

        XCTAssertLessThan(left.x, 0)
        XCTAssertGreaterThan(right.x, 0)
        XCTAssertEqual(left.y, right.y, accuracy: 0.001)
        XCTAssertEqual(left.z, right.z, accuracy: 0.001)
    }

    func testUserFacingHeadMappingMirrorsImageXIntoVRSpace() {
        let mapper = WebcamCameraToTrackingMapper(settings: WebcamTrackingSettings())
        let neutral = SIMD2<Float>(0.5, 0.5)

        let left = mapper.headPosition(
            center: SIMD2<Float>(0.4, 0.5),
            neutralCenter: neutral,
            faceWidth: 0.2,
            neutralFaceWidth: 0.2
        )
        let right = mapper.headPosition(
            center: SIMD2<Float>(0.6, 0.5),
            neutralCenter: neutral,
            faceWidth: 0.2,
            neutralFaceWidth: 0.2
        )

        XCTAssertGreaterThan(left.x, 0)
        XCTAssertLessThan(right.x, 0)
        XCTAssertEqual(left.y, right.y, accuracy: 0.001)
        XCTAssertEqual(left.z, right.z, accuracy: 0.001)
    }

    func testRawCameraHeadMappingKeepsImageXInCameraSpace() {
        var settings = WebcamTrackingSettings()
        settings.cameraFacing = .rawCamera
        let mapper = WebcamCameraToTrackingMapper(settings: settings)
        let neutral = SIMD2<Float>(0.5, 0.5)

        let left = mapper.headPosition(
            center: SIMD2<Float>(0.4, 0.5),
            neutralCenter: neutral,
            faceWidth: 0.2,
            neutralFaceWidth: 0.2
        )
        let right = mapper.headPosition(
            center: SIMD2<Float>(0.6, 0.5),
            neutralCenter: neutral,
            faceWidth: 0.2,
            neutralFaceWidth: 0.2
        )

        XCTAssertLessThan(left.x, 0)
        XCTAssertGreaterThan(right.x, 0)
        XCTAssertEqual(left.y, right.y, accuracy: 0.001)
        XCTAssertEqual(left.z, right.z, accuracy: 0.001)
    }

    func testHeadYawDoesNotMoveHandOrControllerOrigin() {
        let head = WebcamHeadPose(
            position: SIMD3<Float>(0, 1.6, 0),
            orientation: WebcamTrackingMath.headQuaternion(
                fromYawPitchRoll: SIMD3<Float>(.pi / 2, 0, 0)
            ),
            yawPitchRoll: SIMD3<Float>(.pi / 2, 0, 0)
        )
        let hand = makePositionOnlyHand(
            isLeft: true,
            wrist: SIMD3<Float>(0, 1.2, -0.5)
        )

        var settings = WebcamTrackingSettings()
        settings.controllerRotationOffsetDegrees = .zero
        let packet = WebcamGestureMapper.makeTrackingPacket(
            basePacket: basePacket(),
            snapshot: snapshot(head: head, leftHand: hand),
            outputMode: .controllersFromHands,
            headTrackingEnabled: true,
            settings: settings
        )

        XCTAssertEqual(packet.leftControllerPos.0, hand.wristPosition.x, accuracy: 0.001)
        XCTAssertEqual(packet.leftControllerPos.1, hand.wristPosition.y, accuracy: 0.001)
        XCTAssertEqual(packet.leftControllerPos.2, hand.wristPosition.z, accuracy: 0.001)
    }

    func testHandPacketKeepsJointsIndependentFromHeadYaw() {
        let yaw = Float.pi / 3
        let head = WebcamHeadPose(
            position: SIMD3<Float>(0, 1.6, 0),
            orientation: WebcamTrackingMath.headQuaternion(fromYawPitchRoll: SIMD3<Float>(yaw, 0, 0)),
            yawPitchRoll: SIMD3<Float>(yaw, 0, 0)
        )
        let left = makePositionOnlyHand(isLeft: true, wrist: SIMD3<Float>(-0.25, 1.2, -0.3))
        let packet = WebcamGestureMapper.makeTrackingPacket(
            basePacket: basePacket(),
            snapshot: snapshot(head: head, leftHand: left),
            outputMode: .hands,
            headTrackingEnabled: true
        )

        let wrist = joint(1, in: packet.leftHandJoints)
        XCTAssertEqual(wrist.x, left.wristPosition.x, accuracy: 0.001)
        XCTAssertEqual(wrist.y, left.wristPosition.y, accuracy: 0.001)
        XCTAssertEqual(wrist.z, left.wristPosition.z, accuracy: 0.001)
    }

    func testControllerBasisNeutralPalmMatchesKeyboardControllerOrientation() {
        let neutral = WebcamControllerBasis.neutralPalmFacingCameraRotation(isLeft: true)
        let rotation = WebcamControllerBasis.controllerRotation(
            handRotation: neutral,
            isLeft: true
        )
        let aim = WebcamControllerBasis.aimDirection(for: rotation)

        XCTAssertIdentity(rotation)
        XCTAssertEqual(aim.x, 0, accuracy: 0.001)
        XCTAssertEqual(aim.y, 0, accuracy: 0.001)
        XCTAssertEqual(aim.z, -1, accuracy: 0.001)
    }

    func testControllerRotationOffsetIsAppliedAfterStableBasis() {
        let neutral = WebcamControllerBasis.neutralPalmFacingCameraRotation(isLeft: true)
        let hand = makeHand(
            isLeft: true,
            indexPinched: false,
            middleCurl: .open,
            ringCurl: .open,
            pinkyCurl: .open,
            wristRotation: neutral
        )
        var settings = WebcamTrackingSettings()
        settings.controllerRotationOffsetDegrees = SIMD3<Float>(0, 180, 0)

        let packet = WebcamGestureMapper.makeTrackingPacket(
            basePacket: basePacket(),
            snapshot: snapshot(leftHand: hand),
            outputMode: .controllersFromHands,
            headTrackingEnabled: false,
            settings: settings
        )

        let rotation = simd_quatf(
            ix: packet.leftControllerRot.0,
            iy: packet.leftControllerRot.1,
            iz: packet.leftControllerRot.2,
            r: packet.leftControllerRot.3
        )
        let aim = WebcamControllerBasis.aimDirection(for: rotation)

        XCTAssertEqual(packet.leftControllerRot.0, 0, accuracy: 0.001)
        XCTAssertEqual(abs(packet.leftControllerRot.1), 1, accuracy: 0.001)
        XCTAssertEqual(packet.leftControllerRot.2, 0, accuracy: 0.001)
        XCTAssertEqual(abs(packet.leftControllerRot.3), 0, accuracy: 0.001)
        XCTAssertEqual(aim.z, 1, accuracy: 0.001)
    }

    func testRawCameraYawKeepsSignForHeadPose() {
        let yaw90 = simd_quatf(angle: .pi / 2, axis: SIMD3<Float>(0, 1, 0))
        let expected = Float(0.70710678)
        let packet = WebcamGestureMapper.makeTrackingPacket(
            basePacket: basePacket(),
            snapshot: snapshot(head: WebcamHeadPose(position: SIMD3<Float>(0, 1.6, -0.5), orientation: yaw90)),
            outputMode: .hands,
            headTrackingEnabled: true
        )

        XCTAssertEqual(packet.headOrientation.0, 0, accuracy: 0.001)
        XCTAssertEqual(packet.headOrientation.1, expected, accuracy: 0.001)
        XCTAssertEqual(packet.headOrientation.2, 0, accuracy: 0.001)
        XCTAssertEqual(packet.headOrientation.3, expected, accuracy: 0.001)
    }

    func testControllerPoseIgnoresUnstableHandRotation() {
        var settings = WebcamTrackingSettings()
        settings.controllerRotationOffsetDegrees = .zero
        let yaw90 = simd_quatf(angle: .pi / 2, axis: SIMD3<Float>(0, 1, 0))
        let neutral = WebcamControllerBasis.neutralPalmFacingCameraRotation(isLeft: true)
        let neutralHand = makeHand(
            isLeft: true,
            indexPinched: false,
            middleCurl: .open,
            ringCurl: .open,
            pinkyCurl: .open,
            wristRotation: neutral
        )
        let unstableHand = makeHand(
            isLeft: true,
            indexPinched: false,
            middleCurl: .open,
            ringCurl: .open,
            pinkyCurl: .open,
            wristRotation: yaw90 * neutral
        )
        let packet = WebcamGestureMapper.makeTrackingPacket(
            basePacket: basePacket(),
            snapshot: snapshot(leftHand: neutralHand),
            outputMode: .controllersFromHands,
            headTrackingEnabled: false,
            settings: settings
        )
        let unstablePacket = WebcamGestureMapper.makeTrackingPacket(
            basePacket: basePacket(),
            snapshot: snapshot(leftHand: unstableHand),
            outputMode: .controllersFromHands,
            headTrackingEnabled: false,
            settings: settings
        )

        XCTAssertEqual(packet.leftControllerRot.0, unstablePacket.leftControllerRot.0, accuracy: 0.001)
        XCTAssertEqual(packet.leftControllerRot.1, unstablePacket.leftControllerRot.1, accuracy: 0.001)
        XCTAssertEqual(packet.leftControllerRot.2, unstablePacket.leftControllerRot.2, accuracy: 0.001)
        XCTAssertEqual(packet.leftControllerRot.3, unstablePacket.leftControllerRot.3, accuracy: 0.001)
    }

    func testCameraRotationMappingKeepsYawPitchRollSigns() {
        let pitch = simd_quatf(angle: .pi / 4, axis: SIMD3<Float>(1, 0, 0))
        let yaw = simd_quatf(angle: .pi / 5, axis: SIMD3<Float>(0, 1, 0))
        let roll = simd_quatf(angle: .pi / 6, axis: SIMD3<Float>(0, 0, 1))
        let rotation = pitch * yaw * roll
        let converted = WebcamCameraToTrackingMapper.cameraRotationToTracking(rotation)
        let expected = simd_normalize(rotation)

        XCTAssertEqual(converted.imag.x, expected.imag.x, accuracy: 0.001)
        XCTAssertEqual(converted.imag.y, expected.imag.y, accuracy: 0.001)
        XCTAssertEqual(converted.imag.z, expected.imag.z, accuracy: 0.001)
        XCTAssertEqual(converted.real, expected.real, accuracy: 0.001)
    }

    func testHandRotationMappingKeepsControllerAxes() {
        let pitch = simd_quatf(angle: .pi / 4, axis: SIMD3<Float>(1, 0, 0))
        let yaw = simd_quatf(angle: .pi / 5, axis: SIMD3<Float>(0, 1, 0))
        let roll = simd_quatf(angle: .pi / 6, axis: SIMD3<Float>(0, 0, 1))
        let rotation = pitch * yaw * roll
        let converted = WebcamGestureMapper.handToTrackingRotation(rotation)
        let expected = simd_normalize(rotation)

        XCTAssertEqual(converted.imag.x, expected.imag.x, accuracy: 0.001)
        XCTAssertEqual(converted.imag.y, expected.imag.y, accuracy: 0.001)
        XCTAssertEqual(converted.imag.z, expected.imag.z, accuracy: 0.001)
        XCTAssertEqual(converted.real, expected.real, accuracy: 0.001)
    }

    func testWebcamDefaultsMatchDesktopCalibrationBaseline() {
        let settings = WebcamTrackingSettings()

        XCTAssertEqual(settings.cameraFacing, .userFacing)
        XCTAssertEqual(settings.headPositionInterpolation, SIMD3<Float>(0.1, 0.1, 0.1))
        XCTAssertEqual(settings.headRotationInterpolation, SIMD3<Float>(0.3, 0.3, 0.3))
        XCTAssertEqual(settings.handYOffset, 0.5, accuracy: 0.001)
        XCTAssertEqual(settings.handDepthScale, 1.0, accuracy: 0.001)
        XCTAssertEqual(settings.handDepthOffset, 0.0, accuracy: 0.001)
        XCTAssertEqual(settings.handDepthSmoothing, 0.35, accuracy: 0.001)
        XCTAssertEqual(settings.controllerRotationOffsetDegrees, SIMD3<Float>(90, 0, 0))
    }

    func testUserFacingVisionHandednessMirrorsIntoVRSide() {
        XCTAssertFalse(WebcamCameraToTrackingMapper.trackingHandIsLeft(
            visionHandedness: .left,
            wristNormalizedX: 0.25,
            cameraFacing: .userFacing
        ))
        XCTAssertTrue(WebcamCameraToTrackingMapper.trackingHandIsLeft(
            visionHandedness: .right,
            wristNormalizedX: 0.75,
            cameraFacing: .userFacing
        ))
        XCTAssertTrue(WebcamCameraToTrackingMapper.trackingHandIsLeft(
            visionHandedness: .left,
            wristNormalizedX: 0.25,
            cameraFacing: .rawCamera
        ))
        XCTAssertFalse(WebcamCameraToTrackingMapper.trackingHandIsLeft(
            visionHandedness: .right,
            wristNormalizedX: 0.75,
            cameraFacing: .rawCamera
        ))
    }

    func testHandednessFallbackUsesWristXAndCameraFacing() {
        XCTAssertFalse(WebcamCameraToTrackingMapper.trackingHandIsLeft(
            visionHandedness: .unknown,
            wristNormalizedX: 0.25,
            cameraFacing: .userFacing
        ))
        XCTAssertTrue(WebcamCameraToTrackingMapper.trackingHandIsLeft(
            visionHandedness: .unknown,
            wristNormalizedX: 0.75,
            cameraFacing: .userFacing
        ))
        XCTAssertTrue(WebcamCameraToTrackingMapper.trackingHandIsLeft(
            visionHandedness: .unknown,
            wristNormalizedX: 0.25,
            cameraFacing: .rawCamera
        ))
        XCTAssertFalse(WebcamCameraToTrackingMapper.trackingHandIsLeft(
            visionHandedness: .unknown,
            wristNormalizedX: 0.75,
            cameraFacing: .rawCamera
        ))
    }

    func testUserFacingPhysicalRightHandFromImageLeftWritesRightHandPayload() {
        let mapper = WebcamCameraToTrackingMapper(settings: WebcamTrackingSettings())
        let wrist = mapper.handPoint(normalizedPoint: SIMD2<Float>(0.25, 0.5), depth: 0.8)
        let isLeft = WebcamCameraToTrackingMapper.trackingHandIsLeft(
            visionHandedness: .left,
            wristNormalizedX: 0.25,
            cameraFacing: .userFacing
        )
        let hand = makePositionOnlyHand(isLeft: isLeft, wrist: wrist)

        let packet = WebcamGestureMapper.makeTrackingPacket(
            basePacket: basePacket(),
            snapshot: snapshot(rightHand: hand),
            outputMode: .hands,
            headTrackingEnabled: false
        )

        XCTAssertFalse(isLeft)
        XCTAssertEqual(packet.trackingFlags & TrackingFlagsValues.leftHandActive, 0)
        XCTAssertNotEqual(packet.trackingFlags & TrackingFlagsValues.rightHandActive, 0)
        XCTAssertGreaterThan(joint(1, in: packet.rightHandJoints).x, 0)
    }

    func testUserFacingPhysicalRightHandFromImageLeftWritesRightControllerPose() {
        let mapper = WebcamCameraToTrackingMapper(settings: WebcamTrackingSettings())
        let wrist = mapper.handPoint(normalizedPoint: SIMD2<Float>(0.25, 0.5), depth: 0.8)
        let isLeft = WebcamCameraToTrackingMapper.trackingHandIsLeft(
            visionHandedness: .left,
            wristNormalizedX: 0.25,
            cameraFacing: .userFacing
        )
        let hand = makePositionOnlyHand(isLeft: isLeft, wrist: wrist)

        let packet = WebcamGestureMapper.makeTrackingPacket(
            basePacket: basePacket(),
            snapshot: snapshot(rightHand: hand),
            outputMode: .controllersFromHands,
            headTrackingEnabled: false
        )

        XCTAssertFalse(isLeft)
        XCTAssertEqual(packet.trackingFlags & TrackingFlagsValues.leftControllerActive, 0)
        XCTAssertNotEqual(packet.trackingFlags & TrackingFlagsValues.rightControllerActive, 0)
        XCTAssertGreaterThan(packet.rightControllerPos.0, 0)
    }

    func testMovementDeadzoneKeepsSmallPositionChangesStable() {
        let previous = SIMD3<Float>(0, 1.6, -0.5)
        let raw = SIMD3<Float>(0.005, 1.61, -0.54)
        let smoothed = WebcamTrackingMath.smoothVector(
            previous: previous,
            raw: raw,
            interpolation: SIMD3<Float>(1, 1, 1),
            deadzone: 0.015
        )

        XCTAssertEqual(smoothed.x, previous.x, accuracy: 0.001)
        XCTAssertEqual(smoothed.y, previous.y, accuracy: 0.001)
        XCTAssertEqual(smoothed.z, raw.z, accuracy: 0.001)
    }

    func testHeadRotationConstraintsApplyDeadzoneAndLimits() {
        var settings = WebcamTrackingSettings()
        settings.headRotationDeadzoneDegrees = 2
        settings.headRotationLimitDegrees = SIMD3<Float>(30, 20, 5)

        let constrained = WebcamTrackingMath.constrainedHeadEuler(
            yawPitchRoll: SIMD3<Float>(
                WebcamTrackingMath.radians(40),
                WebcamTrackingMath.radians(1),
                WebcamTrackingMath.radians(-12)
            ),
            settings: settings
        )

        XCTAssertEqual(constrained.x, WebcamTrackingMath.radians(30), accuracy: 0.001)
        XCTAssertEqual(constrained.y, 0, accuracy: 0.001)
        XCTAssertEqual(constrained.z, WebcamTrackingMath.radians(-5), accuracy: 0.001)
    }

    func testHeadRotationInterpolationIsPerAxis() {
        let previous = SIMD3<Float>(0, 0, 0)
        let raw = SIMD3<Float>(
            WebcamTrackingMath.radians(20),
            WebcamTrackingMath.radians(10),
            WebcamTrackingMath.radians(4)
        )
        let smoothed = WebcamTrackingMath.smoothAngleVector(
            previous: previous,
            raw: raw,
            interpolation: SIMD3<Float>(0.5, 1, 0),
            deadzone: 0
        )

        XCTAssertEqual(smoothed.x, WebcamTrackingMath.radians(10), accuracy: 0.001)
        XCTAssertEqual(smoothed.y, WebcamTrackingMath.radians(10), accuracy: 0.001)
        XCTAssertEqual(smoothed.z, 0, accuracy: 0.001)
    }

    private enum FingerCurl {
        case open
        case closed
    }

    private func basePacket() -> TrackingPacket {
        var packet = TrackingPacket()
        packet.timestampNs = 1
        packet.headPosition = (0, 1.6, 0)
        packet.headOrientation = (0, 0, 0, 1)
        packet.trackingFlags =
            TrackingFlagsValues.leftControllerActive |
            TrackingFlagsValues.rightControllerActive
        packet.leftTrigger = 0.4
        packet.rightTrigger = 0.4
        packet.leftGrip = 0.4
        packet.rightGrip = 0.4
        packet.buttonState = ButtonFlags.leftTrigger | ButtonFlags.rightTrigger
        return packet
    }

    private func snapshot(
        head: WebcamHeadPose? = nil,
        leftHand: WebcamTrackedHand? = nil,
        rightHand: WebcamTrackedHand? = nil
    ) -> WebcamTrackingSnapshot {
        WebcamTrackingSnapshot(
            timestampNs: 2,
            head: head,
            leftHand: leftHand,
            rightHand: rightHand
        )
    }

    private func makeHand(
        isLeft: Bool,
        indexPinched: Bool,
        middleCurl: FingerCurl,
        ringCurl: FingerCurl,
        pinkyCurl: FingerCurl,
        wristRotation: simd_quatf = simd_quatf(ix: 0, iy: 0, iz: 0, r: 1)
    ) -> WebcamTrackedHand {
        var joints = Array(repeating: SIMD3<Float>(0, 0, -0.5), count: OXRProtocol.handJointCount)
        joints[0] = SIMD3<Float>(0.00, 1.20, -0.50)
        joints[1] = SIMD3<Float>(0.00, 1.08, -0.50)

        joints[5] = SIMD3<Float>(indexPinched ? -0.02 : -0.15, 1.27, -0.50)
        joints[7] = SIMD3<Float>(-0.05, 1.26, -0.50)
        joints[10] = SIMD3<Float>(indexPinched ? -0.015 : -0.05, indexPinched ? 1.275 : 1.44, -0.50)

        joints[12] = SIMD3<Float>(0.00, 1.27, -0.50)
        joints[15] = fingerTip(x: 0.00, curl: middleCurl)
        joints[17] = SIMD3<Float>(0.04, 1.25, -0.50)
        joints[20] = fingerTip(x: 0.04, curl: ringCurl)
        joints[22] = SIMD3<Float>(0.08, 1.23, -0.50)
        joints[25] = fingerTip(x: 0.08, curl: pinkyCurl)

        return WebcamTrackedHand(
            isLeft: isLeft,
            joints: joints,
            wristPosition: joints[1],
            wristRotation: wristRotation
        )
    }

    private func makePositionOnlyHand(isLeft: Bool, wrist: SIMD3<Float>) -> WebcamTrackedHand {
        WebcamTrackedHand(
            isLeft: isLeft,
            joints: Array(repeating: wrist, count: OXRProtocol.handJointCount),
            wristPosition: wrist,
            wristRotation: WebcamControllerBasis.neutralPalmFacingCameraRotation(isLeft: isLeft)
        )
    }

    private func fingerTip(x: Float, curl: FingerCurl) -> SIMD3<Float> {
        switch curl {
        case .open:
            return SIMD3<Float>(x, 1.48, -0.50)
        case .closed:
            return SIMD3<Float>(x, 1.21, -0.50)
        }
    }

    private func joint(_ index: Int, in data: HandJointData) -> SIMD3<Float> {
        withUnsafeBytes(of: data.data) { buffer in
            let values = buffer.bindMemory(to: Float.self)
            let base = index * 4
            return SIMD3<Float>(values[base], values[base + 1], values[base + 2])
        }
    }

    private func XCTAssertIdentity(
        _ rotation: simd_quatf,
        accuracy: Float = 0.001,
        file: StaticString = #filePath,
        line: UInt = #line
    ) {
        let normalized = simd_normalize(rotation)
        XCTAssertEqual(normalized.imag.x, 0, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(normalized.imag.y, 0, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(normalized.imag.z, 0, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(normalized.real, 1, accuracy: accuracy, file: file, line: line)
    }
}
