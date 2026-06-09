// SPDX-License-Identifier: MPL-2.0

import Foundation
import OXRSysStreaming
import simd

enum WebcamTrackingOutputMode: String, CaseIterable, Identifiable, Sendable {
    case hands = "Hands"
    case controllersFromHands = "Controllers from Hands"

    var id: String { rawValue }
}

enum WebcamTrackingSpace: String, CaseIterable, Identifiable, Sendable {
    case local = "Local"
    case localFloor = "Local Floor"
    case stage = "Stage"

    var id: String { rawValue }
}

enum WebcamCameraFacing: String, CaseIterable, Identifiable, Sendable {
    case userFacing = "User-facing Webcam"
    case rawCamera = "Raw Camera"

    var id: String { rawValue }
}

enum WebcamVisionHandedness: Sendable, Equatable {
    case left
    case right
    case unknown
}

struct WebcamTrackingSettings: Sendable {
    var trackingSpace: WebcamTrackingSpace = .localFloor
    var cameraFacing: WebcamCameraFacing = .userFacing
    var cameraY: Float = 1.6
    var headOffset: SIMD3<Float> = .zero
    var handYOffset: Float = 0.5
    var headPositionInterpolation: SIMD3<Float> = SIMD3<Float>(0.1, 0.1, 0.1)
    var headRotationInterpolation: SIMD3<Float> = SIMD3<Float>(0.3, 0.3, 0.3)
    var handPositionInterpolation: SIMD3<Float> = SIMD3<Float>(0.25, 0.25, 0.25)
    var handRotationInterpolation: Float = 0.25
    var handDepthScale: Float = 1.0
    var handDepthOffset: Float = 0.0
    var handDepthSmoothing: Float = 0.35
    var movementDeadzone: Float = 0.015
    var headRotationDeadzoneDegrees: Float = 1.5
    var headRotationLimitDegrees: SIMD3<Float> = SIMD3<Float>(45, 35, 8)
    var controllerRotationOffsetDegrees: SIMD3<Float> = SIMD3<Float>(90, 0, 0)

    static let defaultHandBaseYOffset: Float = -0.4

    func yInTrackingSpace(_ y: Float) -> Float {
        switch trackingSpace {
        case .local:
            return y - cameraY
        case .localFloor, .stage:
            return y
        }
    }
}

struct WebcamCameraToTrackingMapper: Sendable {
    var settings: WebcamTrackingSettings

    func handPoint(normalizedPoint: SIMD2<Float>?, depth: Float) -> SIMD3<Float> {
        let handBaseY = settings.cameraY +
            WebcamTrackingSettings.defaultHandBaseYOffset +
            settings.handYOffset
        let z = depth - 1.25

        guard let normalizedPoint else {
            return SIMD3<Float>(0, settings.yInTrackingSpace(handBaseY), z)
        }

        let rawY = handBaseY + (normalizedPoint.y - 0.5) * 1.45 * depth
        return SIMD3<Float>(
            trackingX(normalizedX: normalizedPoint.x, depth: depth),
            settings.yInTrackingSpace(rawY),
            z
        )
    }

    func headPosition(
        center: SIMD2<Float>,
        neutralCenter: SIMD2<Float>,
        faceWidth: Float,
        neutralFaceWidth: Float
    ) -> SIMD3<Float> {
        let rawCameraY = settings.cameraY + (center.y - neutralCenter.y) * 1.2
        return SIMD3<Float>(
            trackingXDelta(normalizedX: center.x, neutralX: neutralCenter.x, scale: 1.8),
            settings.yInTrackingSpace(rawCameraY),
            (neutralFaceWidth - faceWidth) * 3.2
        ) + settings.headOffset
    }

    func headYawPitchRoll(
        yawPitchRoll: SIMD3<Float>,
        neutralYawPitchRoll: SIMD3<Float>
    ) -> SIMD3<Float> {
        WebcamTrackingMath.constrainedHeadEuler(
            yawPitchRoll: yawPitchRoll - neutralYawPitchRoll,
            settings: settings
        )
    }

    static func cameraRotationToTracking(_ rotation: simd_quatf) -> simd_quatf {
        simd_normalize(rotation)
    }

    static func handRotationToTracking(_ rotation: simd_quatf) -> simd_quatf {
        simd_normalize(rotation)
    }

    func trackingX(normalizedX: Float, depth: Float) -> Float {
        trackingXDelta(normalizedX: normalizedX, neutralX: 0.5, scale: 2.0 * depth)
    }

    func trackingXDelta(normalizedX: Float, neutralX: Float, scale: Float) -> Float {
        let cameraDelta = normalizedX - neutralX
        switch settings.cameraFacing {
        case .userFacing:
            return -cameraDelta * scale
        case .rawCamera:
            return cameraDelta * scale
        }
    }

    static func trackingHandIsLeft(
        visionHandedness: WebcamVisionHandedness,
        wristNormalizedX: Float,
        cameraFacing: WebcamCameraFacing
    ) -> Bool {
        switch visionHandedness {
        case .left:
            return cameraFacing == .rawCamera
        case .right:
            return cameraFacing == .userFacing
        case .unknown:
            switch cameraFacing {
            case .userFacing:
                return wristNormalizedX > 0.5
            case .rawCamera:
                return wristNormalizedX < 0.5
            }
        }
    }
}

enum WebcamControllerBasis {
    static func controllerRotation(
        for hand: WebcamTrackedHand,
        rotationOffsetDegrees: SIMD3<Float>
    ) -> simd_quatf {
        let base = stableControllerRotation(for: hand)
        return simd_normalize(base * WebcamTrackingMath.eulerOffsetQuaternion(degrees: rotationOffsetDegrees))
    }

    static func stableControllerRotation(for hand: WebcamTrackedHand) -> simd_quatf {
        let roll = boundedScreenRoll(for: hand)
        return simd_quatf(angle: roll, axis: SIMD3<Float>(0, 0, -1))
    }

    static func controllerRotation(
        handRotation: simd_quatf,
        isLeft: Bool
    ) -> simd_quatf {
        simd_normalize(handRotation * simd_inverse(neutralPalmFacingCameraRotation(isLeft: isLeft)))
    }

    static func neutralPalmFacingCameraRotation(isLeft: Bool) -> simd_quatf {
        let right = SIMD3<Float>(isLeft ? -1 : 1, 0, 0)
        let up = SIMD3<Float>(0, 0, isLeft ? 1 : -1)
        let forward = SIMD3<Float>(0, 1, 0)
        return simd_quatf(simd_float3x3(columns: (right, up, forward)))
    }

    static func aimDirection(for rotation: simd_quatf) -> SIMD3<Float> {
        simd_normalize(simd_normalize(rotation).act(SIMD3<Float>(0, 0, -1)))
    }

    private static func boundedScreenRoll(for hand: WebcamTrackedHand) -> Float {
        let wrist = joint(1, in: hand.joints)
        let middleMCP = joint(12, in: hand.joints)
        let screenUp = middleMCP - wrist
        let xyLength = simd_length(SIMD2<Float>(screenUp.x, screenUp.y))
        guard xyLength > 0.035 else { return 0 }

        let rawRoll = atan2(screenUp.x, max(abs(screenUp.y), 0.001))
        return min(max(rawRoll * 0.35, -.pi / 8), .pi / 8)
    }

    private static func joint(_ index: Int, in joints: [SIMD3<Float>]) -> SIMD3<Float> {
        guard joints.indices.contains(index) else { return .zero }
        return joints[index]
    }
}

enum WebcamTrackingMath {
    static func clampedUnitVector(_ value: SIMD3<Float>) -> SIMD3<Float> {
        SIMD3<Float>(
            clamp(value.x, 0, 1),
            clamp(value.y, 0, 1),
            clamp(value.z, 0, 1)
        )
    }

    static func smoothVector(
        previous: SIMD3<Float>,
        raw: SIMD3<Float>,
        interpolation: SIMD3<Float>,
        deadzone: Float
    ) -> SIMD3<Float> {
        let adjusted = SIMD3<Float>(
            abs(raw.x - previous.x) < deadzone ? previous.x : raw.x,
            abs(raw.y - previous.y) < deadzone ? previous.y : raw.y,
            abs(raw.z - previous.z) < deadzone ? previous.z : raw.z
        )
        return previous + (adjusted - previous) * clampedUnitVector(interpolation)
    }

    static func smoothAngleVector(
        previous: SIMD3<Float>,
        raw: SIMD3<Float>,
        interpolation: SIMD3<Float>,
        deadzone: Float
    ) -> SIMD3<Float> {
        let adjusted = SIMD3<Float>(
            abs(raw.x - previous.x) < deadzone ? previous.x : raw.x,
            abs(raw.y - previous.y) < deadzone ? previous.y : raw.y,
            abs(raw.z - previous.z) < deadzone ? previous.z : raw.z
        )
        return previous + (adjusted - previous) * clampedUnitVector(interpolation)
    }

    static func constrainedHeadEuler(
        yawPitchRoll: SIMD3<Float>,
        settings: WebcamTrackingSettings
    ) -> SIMD3<Float> {
        let deadzone = radians(settings.headRotationDeadzoneDegrees)
        let limits = SIMD3<Float>(
            radians(settings.headRotationLimitDegrees.x),
            radians(settings.headRotationLimitDegrees.y),
            radians(settings.headRotationLimitDegrees.z)
        )
        return SIMD3<Float>(
            clamp(applyDeadzone(yawPitchRoll.x, deadzone), -limits.x, limits.x),
            clamp(applyDeadzone(yawPitchRoll.y, deadzone), -limits.y, limits.y),
            clamp(applyDeadzone(yawPitchRoll.z, deadzone), -limits.z, limits.z)
        )
    }

    static func headQuaternion(fromYawPitchRoll yawPitchRoll: SIMD3<Float>) -> simd_quatf {
        simd_quatf(angle: yawPitchRoll.x, axis: SIMD3<Float>(0, 1, 0)) *
            simd_quatf(angle: yawPitchRoll.y, axis: SIMD3<Float>(1, 0, 0)) *
            simd_quatf(angle: yawPitchRoll.z, axis: SIMD3<Float>(0, 0, 1))
    }

    static func eulerAngles(from quaternion: simd_quatf) -> SIMD3<Float> {
        let matrix = simd_float3x3(quaternion)
        let pitch = asin(clamp(-matrix[1][2], -1, 1))
        let yaw = atan2(matrix[0][2], matrix[2][2])
        let roll = atan2(matrix[1][0], matrix[1][1])
        return SIMD3<Float>(yaw, pitch, roll)
    }

    static func sameHemisphere(_ quaternion: simd_quatf, reference: simd_quatf) -> simd_quatf {
        let dot = quaternion.imag.x * reference.imag.x +
            quaternion.imag.y * reference.imag.y +
            quaternion.imag.z * reference.imag.z +
            quaternion.real * reference.real
        guard dot < 0 else { return quaternion }
        return simd_quatf(
            ix: -quaternion.imag.x,
            iy: -quaternion.imag.y,
            iz: -quaternion.imag.z,
            r: -quaternion.real
        )
    }

    static func eulerOffsetQuaternion(degrees: SIMD3<Float>) -> simd_quatf {
        let radians = degrees * (.pi / 180)
        return simd_quatf(angle: radians.y, axis: SIMD3<Float>(0, 1, 0)) *
            simd_quatf(angle: radians.x, axis: SIMD3<Float>(1, 0, 0)) *
            simd_quatf(angle: radians.z, axis: SIMD3<Float>(0, 0, 1))
    }

    static func radians(_ degrees: Float) -> Float {
        degrees * (.pi / 180)
    }

    private static func applyDeadzone(_ value: Float, _ deadzone: Float) -> Float {
        abs(value) < deadzone ? 0 : value
    }

    private static func clamp(_ value: Float, _ lower: Float, _ upper: Float) -> Float {
        min(max(value, lower), upper)
    }
}

struct WebcamHeadPose: Sendable {
    var position: SIMD3<Float>
    var orientation: simd_quatf
    var yawPitchRoll: SIMD3<Float>? = nil
}

struct WebcamTrackedHand: Sendable {
    var isLeft: Bool
    var joints: [SIMD3<Float>]
    var wristPosition: SIMD3<Float>
    var wristRotation: simd_quatf
    var estimatedDepth: Float?

    init(
        isLeft: Bool,
        joints: [SIMD3<Float>],
        wristPosition: SIMD3<Float>? = nil,
        wristRotation: simd_quatf? = nil,
        estimatedDepth: Float? = nil
    ) {
        self.isLeft = isLeft
        if joints.count >= OXRProtocol.handJointCount {
            self.joints = Array(joints.prefix(OXRProtocol.handJointCount))
        } else {
            self.joints = joints + Array(
                repeating: joints.last ?? .zero,
                count: OXRProtocol.handJointCount - joints.count
            )
        }
        self.wristPosition = wristPosition ?? Self.joint(at: 1, in: self.joints)
        self.wristRotation = wristRotation ?? WebcamGestureMapper.estimatedHandRotation(
            isLeft: isLeft,
            joints: self.joints
        )
        self.estimatedDepth = estimatedDepth
    }

    private static func joint(at index: Int, in joints: [SIMD3<Float>]) -> SIMD3<Float> {
        guard joints.indices.contains(index) else { return .zero }
        return joints[index]
    }
}

struct WebcamTrackingSnapshot: Sendable {
    var timestampNs: Int64
    var head: WebcamHeadPose?
    var leftHand: WebcamTrackedHand?
    var rightHand: WebcamTrackedHand?
    var usesCalibratedTrackingSpace: Bool = false

    var isTracking: Bool {
        head != nil || leftHand != nil || rightHand != nil
    }
}

enum WebcamGestureMapper {
    private static let handAndControllerFlags =
        TrackingFlagsValues.leftHandActive |
        TrackingFlagsValues.rightHandActive |
        TrackingFlagsValues.leftControllerActive |
        TrackingFlagsValues.rightControllerActive

    private static let gestureButtonMask =
        ButtonFlags.leftTrigger |
        ButtonFlags.rightTrigger |
        ButtonFlags.leftGrip |
        ButtonFlags.rightGrip

    static let triggerClickThreshold: Float = 0.65
    static let gripClickThreshold: Float = 0.58

    static func makeTrackingPacket(
        basePacket: TrackingPacket,
        snapshot: WebcamTrackingSnapshot?,
        outputMode: WebcamTrackingOutputMode,
        headTrackingEnabled: Bool,
        settings: WebcamTrackingSettings = WebcamTrackingSettings()
    ) -> TrackingPacket {
        var packet = basePacket
        packet.timestampNs = snapshot?.timestampNs ?? packet.timestampNs

        packet.trackingFlags &= ~handAndControllerFlags
        packet.buttonState &= ~gestureButtonMask
        packet.leftTrigger = 0
        packet.rightTrigger = 0
        packet.leftGrip = 0
        packet.rightGrip = 0

        guard let snapshot else { return packet }

        if headTrackingEnabled, let head = snapshot.head {
            let orientation = cameraToTrackingRotation(head.orientation)
            packet.headPosition = (
                head.position.x,
                head.position.y,
                head.position.z
            )
            packet.headOrientation = (
                orientation.imag.x,
                orientation.imag.y,
                orientation.imag.z,
                orientation.real
            )
        }

        switch outputMode {
        case .hands:
            if let left = snapshot.leftHand {
                packet.trackingFlags |= TrackingFlagsValues.leftHandActive
                writeHand(left, to: &packet)
            }
            if let right = snapshot.rightHand {
                packet.trackingFlags |= TrackingFlagsValues.rightHandActive
                writeHand(right, to: &packet)
            }
        case .controllersFromHands:
            if let left = snapshot.leftHand {
                packet.trackingFlags |= TrackingFlagsValues.leftControllerActive
                writeController(left, settings: settings, to: &packet)
            }
            if let right = snapshot.rightHand {
                packet.trackingFlags |= TrackingFlagsValues.rightControllerActive
                writeController(right, settings: settings, to: &packet)
            }
        }

        return packet
    }

    static func gestureValues(for hand: WebcamTrackedHand) -> (trigger: Float, grip: Float) {
        let joints = hand.joints
        let scale = max(palmScale(joints), 0.03)
        let thumbTip = joint(5, joints)
        let indexTip = joint(10, joints)
        let pinchRatio = simd_length(thumbTip - indexTip) / scale
        let trigger = closenessValue(ratio: pinchRatio, closed: 0.28, open: 0.78)

        let middleCurl = fingerCurl(mcp: joint(12, joints), tip: joint(15, joints), joints: joints, scale: scale)
        let ringCurl = fingerCurl(mcp: joint(17, joints), tip: joint(20, joints), joints: joints, scale: scale)
        let pinkyCurl = fingerCurl(mcp: joint(22, joints), tip: joint(25, joints), joints: joints, scale: scale)
        let grip = clamp(middleCurl * 0.45 + ringCurl * 0.45 + pinkyCurl * 0.10, 0, 1)

        return (trigger, grip)
    }

    static func estimatedHandRotation(isLeft: Bool, joints: [SIMD3<Float>]) -> simd_quatf {
        let wrist = joint(1, joints)
        let indexBase = joint(7, joints)
        let littleBase = joint(22, joints)
        let middleTip = joint(15, joints)

        var forward = middleTip - wrist
        if simd_length_squared(forward) < 0.000001 {
            forward = SIMD3<Float>(0, 0, -1)
        }
        forward = simd_normalize(forward)

        var across = indexBase - littleBase
        if simd_length_squared(across) < 0.000001 {
            across = SIMD3<Float>(1, 0, 0)
        }
        across = simd_normalize(across) * (isLeft ? -1 : 1)

        var up = simd_cross(across, forward)
        if simd_length_squared(up) < 0.000001 {
            up = SIMD3<Float>(0, 1, 0)
        }
        up = simd_normalize(up)
        let right = simd_normalize(simd_cross(up, forward))

        return simd_quatf(simd_float3x3(columns: (right, up, forward)))
    }

    private static func writeHand(_ hand: WebcamTrackedHand, to packet: inout TrackingPacket) {
        writeControllerPose(hand, to: &packet)
        for (index, joint) in hand.joints.enumerated().prefix(OXRProtocol.handJointCount) {
            if hand.isLeft {
                packet.leftHandJoints.setJoint(
                    index: index,
                    x: joint.x,
                    y: joint.y,
                    z: joint.z,
                    radius: 0.01
                )
            } else {
                packet.rightHandJoints.setJoint(
                    index: index,
                    x: joint.x,
                    y: joint.y,
                    z: joint.z,
                    radius: 0.01
                )
            }
        }
    }

    private static func writeController(
        _ hand: WebcamTrackedHand,
        settings: WebcamTrackingSettings,
        to packet: inout TrackingPacket
    ) {
        writeControllerPose(
            hand,
            rotationOffsetDegrees: settings.controllerRotationOffsetDegrees,
            to: &packet
        )
        let values = gestureValues(for: hand)

        if hand.isLeft {
            packet.leftTrigger = values.trigger
            packet.leftGrip = values.grip
            if values.trigger >= triggerClickThreshold {
                packet.buttonState |= ButtonFlags.leftTrigger
            }
            if values.grip >= gripClickThreshold {
                packet.buttonState |= ButtonFlags.leftGrip
            }
        } else {
            packet.rightTrigger = values.trigger
            packet.rightGrip = values.grip
            if values.trigger >= triggerClickThreshold {
                packet.buttonState |= ButtonFlags.rightTrigger
            }
            if values.grip >= gripClickThreshold {
                packet.buttonState |= ButtonFlags.rightGrip
            }
        }
    }

    private static func writeControllerPose(
        _ hand: WebcamTrackedHand,
        rotationOffsetDegrees: SIMD3<Float> = .zero,
        to packet: inout TrackingPacket
    ) {
        let position = hand.wristPosition
        let rotation = WebcamControllerBasis.controllerRotation(
            for: hand,
            rotationOffsetDegrees: rotationOffsetDegrees
        )
        if hand.isLeft {
            packet.leftControllerPos = (position.x, position.y, position.z)
            packet.leftControllerRot = (
                rotation.imag.x,
                rotation.imag.y,
                rotation.imag.z,
                rotation.real
            )
        } else {
            packet.rightControllerPos = (position.x, position.y, position.z)
            packet.rightControllerRot = (
                rotation.imag.x,
                rotation.imag.y,
                rotation.imag.z,
                rotation.real
            )
        }
    }

    static func cameraToTrackingRotation(_ rotation: simd_quatf) -> simd_quatf {
        WebcamCameraToTrackingMapper.cameraRotationToTracking(rotation)
    }

    static func handToTrackingRotation(_ rotation: simd_quatf) -> simd_quatf {
        WebcamCameraToTrackingMapper.handRotationToTracking(rotation)
    }

    private static func fingerCurl(
        mcp: SIMD3<Float>,
        tip: SIMD3<Float>,
        joints: [SIMD3<Float>],
        scale: Float
    ) -> Float {
        let palm = joint(0, joints)
        let baseDistance = simd_length(mcp - palm)
        let tipDistance = simd_length(tip - palm)
        let curledDistance = baseDistance + scale * 0.20
        let openDistance = baseDistance + scale * 0.80
        return 1 - smoothStep(edge0: curledDistance, edge1: openDistance, x: tipDistance)
    }

    private static func palmScale(_ joints: [SIMD3<Float>]) -> Float {
        let wristToMiddle = simd_length(joint(1, joints) - joint(12, joints))
        let indexToLittle = simd_length(joint(7, joints) - joint(22, joints))
        return max(wristToMiddle, indexToLittle)
    }

    private static func closenessValue(ratio: Float, closed: Float, open: Float) -> Float {
        1 - smoothStep(edge0: closed, edge1: open, x: ratio)
    }

    private static func smoothStep(edge0: Float, edge1: Float, x: Float) -> Float {
        guard edge0 != edge1 else { return x >= edge1 ? 1 : 0 }
        let t = clamp((x - edge0) / (edge1 - edge0), 0, 1)
        return t * t * (3 - 2 * t)
    }

    private static func joint(_ index: Int, _ joints: [SIMD3<Float>]) -> SIMD3<Float> {
        guard joints.indices.contains(index) else { return .zero }
        return joints[index]
    }

    private static func clamp(_ value: Float, _ lower: Float, _ upper: Float) -> Float {
        min(max(value, lower), upper)
    }
}
