// SPDX-License-Identifier: MPL-2.0

// SimulatorInputManager.swift — Keyboard/mouse input → simulated 6DOF pose.
// Converts physical key events and mouse deltas into a TrackingPacket
// that mimics what a headset client would send via the streaming protocol.

import Foundation
import simd
import OXRSysStreaming

final class SimulatorInputManager {
    // Head state
    private var yaw: Float = 0
    private var pitch: Float = 0
    private var roll: Float = 0
    private var headPosition: SIMD3<Float> = [0, 1.6, 0]

    // Controller positions
    private var leftControllerPos: SIMD3<Float> = [-0.2, 1.3, -0.4]
    private var rightControllerPos: SIMD3<Float> = [0.2, 1.3, -0.4]

    // Button state
    private(set) var leftGrab: Bool = false
    private(set) var rightGrab: Bool = false
    private(set) var menuClick: Bool = false

    // Key states (using physical keycodes for AZERTY compatibility)
    private var keyW = false, keyA = false, keyS = false, keyD = false
    private var keyQ = false, keyE = false
    private var keyArrowUp = false, keyArrowDown = false
    private var keyArrowLeft = false, keyArrowRight = false
    private var leftShift = false, rightShift = false

    // Mouse capture state
    private(set) var mouseCaptured = false

    // Mouse delta accumulator — written on main thread (NSEvent), read on timer thread.
    // Protected by a lock because the DispatchSourceTimer runs on a background queue.
    private var mouseLock = os_unfair_lock()
    private var pendingMouseDX: Float = 0
    private var pendingMouseDY: Float = 0

    // Input mode
    enum InputMode { case controller, handTracking }
    private(set) var inputMode: InputMode = .controller

    // Mobile joystick state (written from UI thread, read in update on timer thread — same lock)
    private var joystickMoveX: Float = 0  // right = +1
    private var joystickMoveY: Float = 0  // forward = +1
    private var joystickLookX: Float = 0  // right = +1
    private var joystickLookY: Float = 0  // down = +1

    // Constants
    private let mouseSensitivity: Float = 0.003
    private let arrowSensitivity: Float = 2.0
    private let rollSensitivity: Float = 1.5
    private let moveSpeed: Float = 2.0
    private let joystickLookSensitivity: Float = 2.5  // rad/s at full deflection

    // MARK: - Mobile joystick (iOS/iPadOS)

    func setMoveJoystick(_ x: Float, _ y: Float) {
        os_unfair_lock_lock(&mouseLock)
        joystickMoveX = x
        joystickMoveY = y
        os_unfair_lock_unlock(&mouseLock)
    }

    func setLookJoystick(_ x: Float, _ y: Float) {
        os_unfair_lock_lock(&mouseLock)
        joystickLookX = x
        joystickLookY = y
        os_unfair_lock_unlock(&mouseLock)
    }

    // MARK: - Key events (using NSEvent.keyCode — physical scancodes)

    func onKeyDown(_ keyCode: UInt16) {
        setKey(keyCode, pressed: true)
    }

    func onKeyUp(_ keyCode: UInt16) {
        setKey(keyCode, pressed: false)
    }

    private func setKey(_ keyCode: UInt16, pressed: Bool) {
        // macOS physical keycodes (same on AZERTY and QWERTY)
        switch keyCode {
        case 13: keyW = pressed        // W position (Z on AZERTY)
        case 0:  keyA = pressed        // A position (Q on AZERTY)
        case 1:  keyS = pressed        // S position
        case 2:  keyD = pressed        // D position
        case 12: keyQ = pressed        // Q position (A on AZERTY) — roll left
        case 14: keyE = pressed        // E position — roll right
        case 126: keyArrowUp = pressed
        case 125: keyArrowDown = pressed
        case 123: keyArrowLeft = pressed
        case 124: keyArrowRight = pressed
        case 56: leftShift = pressed   // Left Shift
        case 60: rightShift = pressed  // Right Shift
        case 3:  leftGrab = pressed    // F position — left grip
        case 5:  rightGrab = pressed   // G position — right grip
        case 53: menuClick = pressed   // Escape — menu
        case 17:                       // T position — toggle mode
            if pressed {
                inputMode = (inputMode == .controller) ? .handTracking : .controller
            }
        default: break
        }
    }

    // MARK: - Mouse events

    func onMouseMotion(deltaX: Float, deltaY: Float) {
        guard mouseCaptured else { return }
        // Accumulate on main thread; drained in update() on the timer thread.
        os_unfair_lock_lock(&mouseLock)
        pendingMouseDX += deltaX
        pendingMouseDY += deltaY
        os_unfair_lock_unlock(&mouseLock)
    }

    func onScroll(deltaY: Float) {
        let headRot = simd_quatf(angle: yaw, axis: [0, 1, 0])
        let forward = headRot.act(SIMD3<Float>(0, 0, -1))
        headPosition += forward * deltaY * 0.5
    }

    func setMouseCaptured(_ captured: Bool) {
        mouseCaptured = captured
        if captured {
            // Discard any stale delta accumulated before capture was enabled
            os_unfair_lock_lock(&mouseLock)
            pendingMouseDX = 0
            pendingMouseDY = 0
            os_unfair_lock_unlock(&mouseLock)
        }
    }

    // MARK: - Per-frame update

    func update(deltaTime: Float) {
        // Drain accumulated mouse deltas + snapshot joystick state (thread-safe)
        os_unfair_lock_lock(&mouseLock)
        let dx = pendingMouseDX;  pendingMouseDX = 0
        let dy = pendingMouseDY;  pendingMouseDY = 0
        let jMoveX = joystickMoveX;  let jMoveY = joystickMoveY
        let jLookX = joystickLookX;  let jLookY = joystickLookY
        os_unfair_lock_unlock(&mouseLock)

        // Mouse look
        yaw -= dx * mouseSensitivity
        pitch -= dy * mouseSensitivity

        // Joystick look (iOS/iPadOS)
        yaw -= jLookX * joystickLookSensitivity * deltaTime
        pitch += jLookY * joystickLookSensitivity * deltaTime

        // Arrow keys → head orientation
        if keyArrowLeft  { yaw += arrowSensitivity * deltaTime }
        if keyArrowRight { yaw -= arrowSensitivity * deltaTime }
        if keyArrowUp    { pitch += arrowSensitivity * deltaTime }
        if keyArrowDown  { pitch -= arrowSensitivity * deltaTime }
        pitch = max(-1.5, min(1.5, pitch))

        // Q/E → roll
        if keyQ { roll += rollSensitivity * deltaTime }
        if keyE { roll -= rollSensitivity * deltaTime }

        // Combine WASD + left joystick for movement
        let headRotYaw = simd_quatf(angle: yaw, axis: [0, 1, 0])
        let forward = headRotYaw.act(SIMD3<Float>(0, 0, -1))
        let right = headRotYaw.act(SIMD3<Float>(1, 0, 0))

        var fwd: Float = jMoveY
        var strafe: Float = jMoveX
        if keyW { fwd += 1 }
        if keyS { fwd -= 1 }
        if keyD { strafe += 1 }
        if keyA { strafe -= 1 }
        fwd = max(-1, min(1, fwd))
        strafe = max(-1, min(1, strafe))

        var moveDir = forward * fwd + right * strafe
        if simd_length(moveDir) > 0.001 {
            moveDir = simd_normalize(moveDir) * moveSpeed * deltaTime

            if leftShift && !rightShift {
                leftControllerPos += moveDir
            } else if rightShift && !leftShift {
                rightControllerPos += moveDir
            } else {
                headPosition += moveDir
            }
        }
    }

    // MARK: - Build tracking packet

    func buildTrackingPacket() -> TrackingPacket {
        var packet = TrackingPacket()

        let now = mach_absolute_time()
        var info = mach_timebase_info_data_t()
        mach_timebase_info(&info)
        packet.timestampNs = Int64(now * UInt64(info.numer) / UInt64(info.denom))

        // Head pose
        packet.headPosition = (headPosition.x, headPosition.y, headPosition.z)
        let headQuat = buildHeadQuaternion()
        packet.headOrientation = (headQuat.imag.x, headQuat.imag.y, headQuat.imag.z, headQuat.real)

        // Controllers — orient same as head for simplicity
        packet.trackingFlags |= TrackingFlagsValues.leftControllerActive
        packet.trackingFlags |= TrackingFlagsValues.rightControllerActive
        packet.leftControllerPos = (leftControllerPos.x, leftControllerPos.y, leftControllerPos.z)
        packet.leftControllerRot = (headQuat.imag.x, headQuat.imag.y, headQuat.imag.z, headQuat.real)
        packet.rightControllerPos = (rightControllerPos.x, rightControllerPos.y, rightControllerPos.z)
        packet.rightControllerRot = (headQuat.imag.x, headQuat.imag.y, headQuat.imag.z, headQuat.real)

        // Buttons
        packet.leftGrip = leftGrab ? 1.0 : 0.0
        packet.rightGrip = rightGrab ? 1.0 : 0.0
        if menuClick {
            packet.buttonState |= ButtonFlags.menu
        }

        return packet
    }

    private func buildHeadQuaternion() -> simd_quatf {
        let yawQ = simd_quatf(angle: yaw, axis: [0, 1, 0])
        let pitchQ = simd_quatf(angle: pitch, axis: [1, 0, 0])
        let rollQ = simd_quatf(angle: roll, axis: [0, 0, 1])
        return yawQ * pitchQ * rollQ
    }
}
