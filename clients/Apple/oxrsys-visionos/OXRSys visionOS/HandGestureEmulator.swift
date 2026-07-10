// SPDX-License-Identifier: MPL-2.0

import OXRSysStreaming
import simd

/// Synthesizes an emulated VR controller from hand-tracking joints so controller-only PCVR games
/// are playable on Vision Pro when no physical spatial controller is present.
///
/// The grip pose comes from the wrist; the analog trigger comes from the thumb↔index pinch
/// distance; digital buttons and grip come from pinch/curl gestures with hysteresis (separate
/// enter/exit thresholds) to avoid chatter and false triggers. Mirrors the ALVR gesture scheme:
/// A/B on the right hand, X/Y on the left, index pinch = trigger, three-finger curl = grip,
/// little-finger pinch (left) = menu. The thumbstick arm-gesture is intentionally left unmapped
/// for now (too jittery without tuning on-device).
struct HandGestureEmulator {
    enum Hand { case left, right }

    // Pinch (thumb tip ↔ finger tip) thresholds in metres, with hysteresis.
    private static let pinchEnter: Float = 0.020
    private static let pinchExit: Float = 0.030
    // Analog trigger ramp: 0 at triggerOpen, 1 at pinchEnter.
    private static let triggerOpen: Float = 0.060
    // Grip curl ratio (mean fingertip→wrist distance / palm length), with hysteresis.
    private static let gripEnterRatio: Float = 1.15
    private static let gripExitRatio: Float = 1.35

    // Joint indices into the runtime's 26-joint layout (see VisionTrackingManager.makeJointArray).
    private static let wrist = 1
    private static let thumbTip = 5
    private static let indexTip = 10
    private static let middleTip = 15
    private static let ringTip = 20
    private static let littleTip = 25
    private static let middleKnuckle = 12

    /// Latched per-gesture state (hysteresis) for one hand.
    private struct State {
        var middlePinched = false
        var ringPinched = false
        var littlePinched = false
        var gripped = false
    }

    private var leftState = State()
    private var rightState = State()

    mutating func emulate(from hand: VisionHandState, chirality: Hand) -> VisionControllerState {
        var state = chirality == .left ? leftState : rightState
        let j = hand.joints

        let thumb = j[Self.thumbTip]
        func pinch(_ tipIndex: Int, latched: inout Bool) -> Bool {
            let d = simd_distance(thumb, j[tipIndex])
            if latched {
                if d > Self.pinchExit { latched = false }
            } else if d < Self.pinchEnter {
                latched = true
            }
            return latched
        }

        let middle = pinch(Self.middleTip, latched: &state.middlePinched)
        let ring = pinch(Self.ringTip, latched: &state.ringPinched)
        let little = pinch(Self.littleTip, latched: &state.littlePinched)

        // Analog trigger from the thumb↔index distance (continuous, no latch needed).
        let indexDist = simd_distance(thumb, j[Self.indexTip])
        let trigger = simd_clamp(
            (Self.triggerOpen - indexDist) / (Self.triggerOpen - Self.pinchEnter), 0, 1)

        // Grip from middle/ring/little curl: mean fingertip→wrist distance over palm length.
        let wrist = j[Self.wrist]
        let palm = max(simd_distance(wrist, j[Self.middleKnuckle]), 0.0001)
        let curlRatio = (simd_distance(j[Self.middleTip], wrist)
                         + simd_distance(j[Self.ringTip], wrist)
                         + simd_distance(j[Self.littleTip], wrist)) / (3 * palm)
        if state.gripped {
            if curlRatio > Self.gripExitRatio { state.gripped = false }
        } else if curlRatio < Self.gripEnterRatio {
            state.gripped = true
        }

        var buttons: UInt32 = 0
        switch chirality {
        case .right:
            if ring { buttons |= ButtonFlags.a }
            if middle { buttons |= ButtonFlags.b }
        case .left:
            if ring { buttons |= ButtonFlags.x }
            if middle { buttons |= ButtonFlags.y }
            if little { buttons |= ButtonFlags.menu }
        }

        if chirality == .left { leftState = state } else { rightState = state }

        return VisionControllerState(
            position: hand.wristPosition,
            orientation: hand.wristRotation,
            buttonState: buttons,
            trigger: trigger,
            grip: state.gripped ? 1.0 : 0.0,
            thumbstick: SIMD2<Float>(0, 0)
        )
    }

    mutating func reset() {
        leftState = State()
        rightState = State()
    }
}
