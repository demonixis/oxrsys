// SPDX-License-Identifier: MPL-2.0

import ARKit
import Foundation
import GameController
import OXRSysStreaming
import QuartzCore
import simd

/// Orientation correction for emulated (hand-derived) controller poses. The hand frame points
/// +Z toward the fingertips, opposite the OpenXR grip pose's forward (−Z), so an uncorrected
/// controller can appear to point the wrong way. `.meta` applies the flip that matches Touch.
enum ControllerOrientationLayout: String, CaseIterable, Sendable {
    case khronos
    case meta

    var correction: simd_quatf {
        switch self {
        case .khronos:
            return simd_quatf(ix: 0, iy: 0, iz: 0, r: 1) // identity (raw hand frame)
        case .meta:
            return simd_quatf(angle: .pi, axis: SIMD3<Float>(0, 1, 0)) // 180° yaw flip
        }
    }
}

struct VisionHandState: Sendable {
    let wristPosition: SIMD3<Float>
    let wristRotation: simd_quatf
    let joints: [SIMD3<Float>]
}

struct VisionControllerState: Sendable {
    let position: SIMD3<Float>
    let orientation: simd_quatf
    let buttonState: UInt32
    let trigger: Float
    let grip: Float
    let thumbstick: SIMD2<Float>
}

struct VisionTrackingSnapshot: Sendable {
    var position: SIMD3<Float> = .zero
    var orientation: simd_quatf = simd_quatf(ix: 0, iy: 0, iz: 0, r: 1)
    /// Measured head velocities (world frame, m/s and rad/s) from consecutive ARKit samples.
    /// The runtime prefers these over its own finite differencing of received UDP poses for
    /// bounded pose prediction; angular velocity uses the pre-multiply convention it applies.
    var linearVelocity: SIMD3<Float> = .zero
    var angularVelocity: SIMD3<Float> = .zero
    var timestampNs: Int64 = 0
    var isTracking = false
    var leftHand: VisionHandState?
    var rightHand: VisionHandState?
    var leftController: VisionControllerState?
    var rightController: VisionControllerState?
}

final class VisionTrackingManager: @unchecked Sendable {
    // ARKit sessions and data providers are single-use: once a provider stops (e.g. the
    // immersive space closes) the same instance can't be re-run — it returns no anchors.
    // These are recreated by `prepareForNewSession()` before each (re)entry so reconnecting
    // gets live tracking instead of a permanently-stopped provider.
    private var session = ARKitSession()
    private(set) var worldTracking = WorldTrackingProvider()
    private var handTracking = HandTrackingProvider()
    private let queue = DispatchQueue(label: "oxr.visionos.tracking", qos: .userInteractive)

    private var runTask: Task<Void, Never>?
    private var sampleTimer: DispatchSourceTimer?
    private var running = false
    private var accessoryTrackingProvider: Any?
    private var accessoryAnchorLogCounter = 0
    // Accessory anchors from the provider's async update stream, kept fresh per hand ([0]=left,
    // [1]=right). AccessoryTrackingProvider.latestAnchors does NOT update with the controller — it
    // returns the anchor captured at connect — so (unlike hand tracking) the live pose must come
    // from consuming anchorUpdates and predicting the stored anchor to the frame time.
    private let accessoryAnchorLock = NSLock()
    private var storedAccessoryAnchors: [AccessoryAnchor?] = [nil, nil]
    private var accessoryConsumeTask: Task<Void, Never>?
    private var lastHeadOrientation: simd_quatf?

    // Previous sample for head-velocity measurement (all access on `queue`), plus a light EMA so
    // the reported velocities don't carry per-sample tracking noise into the server's prediction.
    private var lastSamplePosition: SIMD3<Float>?
    private var lastSampleOrientation: simd_quatf?
    private var lastSampleTime: Double = 0
    private var smoothedLinearVelocity: SIMD3<Float> = .zero
    private var smoothedAngularVelocity: SIMD3<Float> = .zero

    // Controller emulation from hands / gamepad. All access is on `queue` (with sampleTracking).
    private var gestureEmulator = HandGestureEmulator()
    private var gestureEmulationEnabled = false
    // Compatibility mode: hand poses + a connected gamepad (e.g. Xbox) emulate Touch controllers.
    // Takes priority over gesture emulation when a gamepad is present.
    private var controllerCompatibilityEnabled = false
    private var controllerLayout: ControllerOrientationLayout = .meta

    var onTrackingUpdate: (@Sendable (VisionTrackingSnapshot) -> Void)?

    /// One-time launch probe, logged via NSLog (unified log, so it can be streamed off the device
    /// without an Xcode debug session) to show the spatial-controller failure stage directly:
    /// whether accessory tracking is supported at all, and whether GameController surfaces the
    /// Sense controllers as spatial. Runs at app launch — no streaming/immersive flow required.
    nonisolated(unsafe) private static var launchDiagnosticsStarted = false
    static func logLaunchDiagnostics() {
        guard !launchDiagnosticsStarted else { return }
        launchDiagnosticsStarted = true
        func dump(_ tag: String) {
            let all = GCController.controllers()
            NSLog("[SpatialDiag] \(tag): GCController.controllers()=\(all.count)")
            for c in all {
                var spatial = false
                if #available(visionOS 26.0, *) { spatial = c.productCategory == GCProductCategorySpatialController }
                NSLog("[SpatialDiag]   '\(c.vendorName ?? "?")' category='\(c.productCategory)' spatial=\(spatial)")
            }
        }
        if #available(visionOS 26.0, *) {
            NSLog("[SpatialDiag] AccessoryTrackingProvider.isSupported=\(AccessoryTrackingProvider.isSupported)")
        } else {
            NSLog("[SpatialDiag] visionOS < 26: accessory tracking unavailable")
        }
        dump("launch")
        NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { _ in dump("didConnect") }
        NotificationCenter.default.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { _ in dump("didDisconnect") }
    }

    /// Enable hand-gesture controller emulation (pinch/curl → buttons/trigger/grip, wrist → pose).
    func setGestureEmulationEnabled(_ enabled: Bool) {
        queue.async { [self] in gestureEmulationEnabled = enabled }
    }

    /// Enable hands+gamepad compatibility mode (hand pose + gamepad buttons emulate Touch).
    func setControllerCompatibilityEnabled(_ enabled: Bool) {
        queue.async { [self] in controllerCompatibilityEnabled = enabled }
    }

    /// Select the emulated-controller orientation layout.
    func setControllerLayout(_ layout: ControllerOrientationLayout) {
        queue.async { [self] in controllerLayout = layout }
    }

    func start() {
        queue.async { [self] in
            guard !running else { return }
            running = true

            runTask = Task {
                await runSession()
            }

            let timer = DispatchSource.makeTimerSource(flags: .strict, queue: queue)
            timer.schedule(deadline: .now(), repeating: .nanoseconds(11_111_111), leeway: .microseconds(500))
            timer.setEventHandler { [weak self] in
                self?.sampleTracking()
            }
            sampleTimer = timer
            timer.resume()
        }
    }

    func stop() {
        queue.async { [self] in
            guard running else { return }
            tearDownLocked()
            print("[VisionTracking] Stopped")
        }
    }

    /// Recreate the ARKit session and data providers so the next `start()` runs on fresh,
    /// runnable instances. ARKit providers are single-use — reusing a stopped provider yields
    /// no anchors (no tracking) until the app is relaunched. Call on the main thread before
    /// (re)opening the immersive space so the renderer captures the new world-tracking provider.
    func prepareForNewSession() {
        queue.sync { [self] in
            tearDownLocked()
            session = ARKitSession()
            worldTracking = WorldTrackingProvider()
            handTracking = HandTrackingProvider()
        }
    }

    /// Stop the running session and cancel work. Must be called on `queue`.
    private func tearDownLocked() {
        running = false
        sampleTimer?.cancel()
        sampleTimer = nil
        runTask?.cancel()
        runTask = nil
        session.stop()
        accessoryConsumeTask?.cancel()
        accessoryConsumeTask = nil
        accessoryTrackingProvider = nil
        accessoryAnchorLock.lock()
        storedAccessoryAnchors = [nil, nil]
        accessoryAnchorLock.unlock()
        lastHeadOrientation = nil
        lastSamplePosition = nil
        lastSampleOrientation = nil
        lastSampleTime = 0
        smoothedLinearVelocity = .zero
        smoothedAngularVelocity = .zero
        gestureEmulator.reset()
    }

    private func runSession() async {
        do {
            let providers = try await makeProviders()
            try await session.run(providers)
            print("[VisionTracking] Started")
            if #available(visionOS 26.0, *),
               let accessoryProvider = accessoryTrackingProvider as? AccessoryTrackingProvider {
                accessoryConsumeTask = Task { [weak self] in
                    await self?.consumeAccessoryAnchors(accessoryProvider)
                }
            }
        } catch {
            print("[VisionTracking] Failed to start: \(error)")
        }
    }

    private func makeProviders() async throws -> [any DataProvider] {
        let authorizationTypes = Set(
            WorldTrackingProvider.requiredAuthorizations +
            (HandTrackingProvider.isSupported ? HandTrackingProvider.requiredAuthorizations : []) +
            accessoryAuthorizationTypes()
        )

        if !authorizationTypes.isEmpty {
            let statuses = await session.requestAuthorization(for: Array(authorizationTypes))
            for (type, status) in statuses {
                print("[VisionTracking] Authorization \(type): \(status)")
            }
        }

        var providers: [any DataProvider] = [worldTracking]

        if HandTrackingProvider.isSupported {
            providers.append(handTracking)
        }

        if #available(visionOS 26.0, *),
           AccessoryTrackingProvider.isSupported,
           let accessoryProvider = await makeAccessoryTrackingProvider() {
            accessoryTrackingProvider = accessoryProvider
            providers.append(accessoryProvider)
        } else {
            accessoryTrackingProvider = nil
        }

        return providers
    }

    private func accessoryAuthorizationTypes() -> [ARKitSession.AuthorizationType] {
        if #available(visionOS 26.0, *), AccessoryTrackingProvider.isSupported {
            return AccessoryTrackingProvider.requiredAuthorizations
        }
        return []
    }

    @available(visionOS 26.0, *)
    private func makeAccessoryTrackingProvider() async -> AccessoryTrackingProvider? {
        // Only spatial controllers (e.g. PSVR2 Sense) can be tracked as accessories; a regular
        // gamepad (Xbox) is not one, and Accessory(device:) would just throw for it.
        let controllers = GCController.controllers().filter {
            $0.productCategory == GCProductCategorySpatialController
        }
        print("[VisionTracking] spatial controllers detected: \(controllers.count) "
              + "\(controllers.map { $0.vendorName ?? "?" })")
        guard !controllers.isEmpty else { return nil }

        var accessories: [Accessory] = []
        accessories.reserveCapacity(controllers.count)

        for controller in controllers {
            do {
                accessories.append(try await Accessory(device: controller))
            } catch {
                print("[VisionTracking] Failed to create accessory for controller \(controller.vendorName ?? "Unknown"): \(error)")
            }
        }

        guard !accessories.isEmpty else { return nil }
        print("[VisionTracking] Tracking \(accessories.count) accessory controllers")
        return AccessoryTrackingProvider(accessories: accessories)
    }

    /// Keep a fresh anchor per hand from the provider's async update stream. Required because
    /// `latestAnchors` does not update for accessories — without this the controller pose freezes
    /// at the connect pose. sampleTracking predicts these stored anchors to the frame time.
    @available(visionOS 26.0, *)
    private func consumeAccessoryAnchors(_ provider: AccessoryTrackingProvider) async {
        for await update in provider.anchorUpdates {
            let anchor = update.anchor
            guard let chirality = controllerHandedness(for: anchor) else { continue }
            let hand: Int
            switch chirality {
            case .left: hand = 0
            case .right: hand = 1
            default: continue
            }
            accessoryAnchorLock.lock()
            storedAccessoryAnchors[hand] = (update.event == .removed) ? nil : anchor
            accessoryAnchorLock.unlock()
        }
    }

    private func sampleTracking() {
        guard running else { return }
        let timestamp = CACurrentMediaTime()
        guard let deviceAnchor = worldTracking.queryDeviceAnchor(atTimestamp: timestamp) else { return }

        let transform = deviceAnchor.originFromAnchorTransform
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

        let orientation = stabilized(
            simd_quatf(rotationMatrix),
            previous: lastHeadOrientation
        )
        lastHeadOrientation = orientation

        // Measure head velocity from consecutive ARKit samples on this steady timer — a far
        // cleaner signal than the server finite-differencing poses off jittery UDP arrival.
        var linearVelocity = SIMD3<Float>.zero
        var angularVelocity = SIMD3<Float>.zero
        if deviceAnchor.isTracked {
            if let prevPosition = lastSamplePosition,
               let prevOrientation = lastSampleOrientation {
                let dt = Float(timestamp - lastSampleTime)
                if dt > 0.001, dt < 0.1 {
                    let rawLinear = (position - prevPosition) / dt
                    // World-frame angular velocity from the pre-multiply delta (q_now = Δq·q_prev),
                    // the convention the runtime's PredictOrientationFromVelocity applies.
                    var delta = simd_normalize(orientation * prevOrientation.inverse)
                    if delta.real < 0 { delta = simd_quatf(vector: -delta.vector) } // shortest arc
                    var rawAngular = SIMD3<Float>.zero
                    let imagLength = simd_length(delta.imag)
                    if imagLength > 1e-6 {
                        let angle = 2 * atan2(imagLength, delta.real)
                        rawAngular = (delta.imag / imagLength) * (angle / dt)
                    }
                    // Light EMA (~2 samples): strips per-sample noise without adding real lag.
                    let alpha: Float = 0.5
                    smoothedLinearVelocity = smoothedLinearVelocity * (1 - alpha) + rawLinear * alpha
                    smoothedAngularVelocity = smoothedAngularVelocity * (1 - alpha) + rawAngular * alpha
                }
            }
            lastSamplePosition = position
            lastSampleOrientation = orientation
            lastSampleTime = timestamp
            linearVelocity = smoothedLinearVelocity
            angularVelocity = smoothedAngularVelocity
        } else {
            // Tracking lost: report zero and restart the measurement on reacquisition.
            lastSamplePosition = nil
            lastSampleOrientation = nil
            smoothedLinearVelocity = .zero
            smoothedAngularVelocity = .zero
        }

        var snapshot = VisionTrackingSnapshot(
            position: position,
            orientation: orientation,
            linearVelocity: linearVelocity,
            angularVelocity: angularVelocity,
            timestampNs: Int64(timestamp * 1_000_000_000),
            isTracking: deviceAnchor.isTracked
        )

        let handAnchors = handTracking.latestAnchors
        if let leftHandAnchor = handAnchors.leftHand {
            snapshot.leftHand = makeHandState(from: leftHandAnchor)
        }
        if let rightHandAnchor = handAnchors.rightHand {
            snapshot.rightHand = makeHandState(from: rightHandAnchor)
        }

        if #available(visionOS 26.0, *),
           let accessoryProvider = accessoryTrackingProvider as? AccessoryTrackingProvider {
            accessoryAnchorLock.lock()
            let stored = storedAccessoryAnchors
            accessoryAnchorLock.unlock()

            // Predict each fresh anchor to the frame's timestamp (the instant the head pose is
            // queried for) so the controllers track live rather than freezing at the connect pose.
            if let leftAnchor = stored[0] {
                let tracked = accessoryProvider.predictAnchor(for: leftAnchor, at: timestamp) ?? leftAnchor
                snapshot.leftController = makeControllerState(from: tracked, isLeftHand: true)
            }
            if let rightAnchor = stored[1] {
                let tracked = accessoryProvider.predictAnchor(for: rightAnchor, at: timestamp) ?? rightAnchor
                snapshot.rightController = makeControllerState(from: tracked, isLeftHand: false)
            }

            accessoryAnchorLogCounter += 1
            if accessoryAnchorLogCounter % 180 == 1 {  // ~2 s; a CHANGING Lpos means it is tracking
                let lp = snapshot.leftController?.position
                print("[VisionTracking] accessory: stored L=\(stored[0] != nil) R=\(stored[1] != nil) "
                      + "Lpos=\(lp.map { String(format: "(%.2f, %.2f, %.2f)", $0.x, $0.y, $0.z) } ?? "nil")")
            }
        }

        // Fill controllers for hands without a physical spatial controller. Compatibility mode
        // (hand pose + gamepad buttons) wins when a gamepad is connected; otherwise fall back to
        // gesture emulation. The hand skeleton is still sent alongside.
        let compatGamepad = controllerCompatibilityEnabled ? connectedGamepad() : nil
        if let compatGamepad {
            if snapshot.leftController == nil, let leftHand = snapshot.leftHand {
                snapshot.leftController = applyLayout(makeCompatibilityController(hand: leftHand, gamepad: compatGamepad, isLeft: true))
            }
            if snapshot.rightController == nil, let rightHand = snapshot.rightHand {
                snapshot.rightController = applyLayout(makeCompatibilityController(hand: rightHand, gamepad: compatGamepad, isLeft: false))
            }
        } else if gestureEmulationEnabled {
            if snapshot.leftController == nil, let leftHand = snapshot.leftHand {
                snapshot.leftController = applyLayout(gestureEmulator.emulate(from: leftHand, chirality: .left))
            }
            if snapshot.rightController == nil, let rightHand = snapshot.rightHand {
                snapshot.rightController = applyLayout(gestureEmulator.emulate(from: rightHand, chirality: .right))
            }
        }

        onTrackingUpdate?(snapshot)
    }

    /// Applies the selected orientation-layout correction to an emulated controller's pose.
    private func applyLayout(_ state: VisionControllerState) -> VisionControllerState {
        let corrected = simd_normalize(state.orientation * controllerLayout.correction)
        return VisionControllerState(
            position: state.position,
            orientation: corrected,
            buttonState: state.buttonState,
            trigger: state.trigger,
            grip: state.grip,
            thumbstick: state.thumbstick
        )
    }

    /// First connected non-spatial gamepad (e.g. an Xbox controller) for compatibility mode.
    /// Spatial controllers are skipped — they are handled by the accessory-tracking path.
    private func connectedGamepad() -> GCExtendedGamepad? {
        for controller in GCController.controllers() {
            if #available(visionOS 26.0, *),
               controller.productCategory == GCProductCategorySpatialController {
                continue
            }
            if let gamepad = controller.extendedGamepad {
                return gamepad
            }
        }
        return nil
    }

    /// Emulates one Touch controller: 6DOF pose from the hand wrist, inputs from the gamepad.
    /// Left gets X/Y + menu + left stick/trigger/bumper; right gets A/B + right stick/trigger/bumper.
    private func makeCompatibilityController(hand: VisionHandState,
                                             gamepad: GCExtendedGamepad,
                                             isLeft: Bool) -> VisionControllerState {
        var buttons: UInt32 = 0
        let trigger: Float
        let grip: Float
        let thumbstick: SIMD2<Float>

        if isLeft {
            if gamepad.buttonX.isPressed { buttons |= ButtonFlags.x }
            if gamepad.buttonY.isPressed { buttons |= ButtonFlags.y }
            if gamepad.buttonMenu.isPressed || gamepad.buttonOptions?.isPressed == true {
                buttons |= ButtonFlags.menu
            }
            trigger = gamepad.leftTrigger.value
            grip = gamepad.leftShoulder.value
            thumbstick = SIMD2<Float>(gamepad.leftThumbstick.xAxis.value,
                                      gamepad.leftThumbstick.yAxis.value)
        } else {
            if gamepad.buttonA.isPressed { buttons |= ButtonFlags.a }
            if gamepad.buttonB.isPressed { buttons |= ButtonFlags.b }
            trigger = gamepad.rightTrigger.value
            grip = gamepad.rightShoulder.value
            thumbstick = SIMD2<Float>(gamepad.rightThumbstick.xAxis.value,
                                      gamepad.rightThumbstick.yAxis.value)
        }

        return VisionControllerState(
            position: hand.wristPosition,
            orientation: hand.wristRotation,
            buttonState: buttons,
            trigger: trigger,
            grip: grip,
            thumbstick: thumbstick
        )
    }

    private func stabilized(_ orientation: simd_quatf, previous: simd_quatf?) -> simd_quatf {
        let normalized = simd_normalize(orientation)
        guard let previous else { return normalized }
        return simd_dot(previous.vector, normalized.vector) < 0 ? simd_quatf(vector: -normalized.vector) : normalized
    }

    private func makeHandState(from anchor: HandAnchor) -> VisionHandState? {
        guard anchor.isTracked, let skeleton = anchor.handSkeleton else { return nil }

        let worldFromWrist = anchor.originFromAnchorTransform
        let wristPosition = worldFromWrist.translation

        let joints = makeJointArray(from: anchor, skeleton: skeleton, wristPosition: wristPosition)
        guard joints.count == 26 else { return nil }

        let wristRotation = makeWristRotation(
            wristPosition: wristPosition,
            indexBase: joints[7],
            littleBase: joints[22],
            middleTip: joints[15],
            chirality: anchor.chirality
        )

        return VisionHandState(
            wristPosition: wristPosition,
            wristRotation: wristRotation,
            joints: joints
        )
    }

    private func makeJointArray(from anchor: HandAnchor, skeleton: HandSkeleton, wristPosition: SIMD3<Float>) -> [SIMD3<Float>] {
        func worldPosition(_ name: HandSkeleton.JointName) -> SIMD3<Float> {
            let joint = skeleton.joint(name)
            let worldFromJoint = anchor.originFromAnchorTransform * joint.anchorFromJointTransform
            return worldFromJoint.translation
        }

        func midpoint(_ a: SIMD3<Float>, _ b: SIMD3<Float>) -> SIMD3<Float> {
            (a + b) * 0.5
        }

        let indexKnuckle = worldPosition(.indexFingerKnuckle)
        let middleKnuckle = worldPosition(.middleFingerKnuckle)
        let ringKnuckle = worldPosition(.ringFingerKnuckle)
        let littleKnuckle = worldPosition(.littleFingerKnuckle)

        return [
            midpoint(wristPosition, middleKnuckle),
            wristPosition,
            worldPosition(.thumbKnuckle),
            worldPosition(.thumbIntermediateBase),
            worldPosition(.thumbIntermediateTip),
            worldPosition(.thumbTip),
            midpoint(wristPosition, indexKnuckle),
            indexKnuckle,
            worldPosition(.indexFingerIntermediateBase),
            worldPosition(.indexFingerIntermediateTip),
            worldPosition(.indexFingerTip),
            midpoint(wristPosition, middleKnuckle),
            middleKnuckle,
            worldPosition(.middleFingerIntermediateBase),
            worldPosition(.middleFingerIntermediateTip),
            worldPosition(.middleFingerTip),
            midpoint(wristPosition, ringKnuckle),
            ringKnuckle,
            worldPosition(.ringFingerIntermediateBase),
            worldPosition(.ringFingerIntermediateTip),
            worldPosition(.ringFingerTip),
            midpoint(wristPosition, littleKnuckle),
            littleKnuckle,
            worldPosition(.littleFingerIntermediateBase),
            worldPosition(.littleFingerIntermediateTip),
            worldPosition(.littleFingerTip),
        ]
    }

    private func makeWristRotation(
        wristPosition: SIMD3<Float>,
        indexBase: SIMD3<Float>,
        littleBase: SIMD3<Float>,
        middleTip: SIMD3<Float>,
        chirality: HandAnchor.Chirality
    ) -> simd_quatf {
        let forward = simd_normalize(middleTip - wristPosition)
        let sign: Float = chirality == .left ? -1 : 1
        let across = simd_normalize(indexBase - littleBase) * sign
        let up = simd_normalize(simd_cross(across, forward))
        let right = simd_normalize(simd_cross(forward, up))

        if !forward.allFinite || !across.allFinite || !up.allFinite || !right.allFinite {
            return simd_quatf(ix: 0, iy: 0, iz: 0, r: 1)
        }

        return simd_quatf(simd_float3x3(columns: (right, up, forward)))
    }

    @available(visionOS 26.0, *)
    private func controllerHandedness(for anchor: AccessoryAnchor) -> Accessory.Chirality? {
        if let held = anchor.heldChirality {
            return held
        }
        let inherent = anchor.accessory.inherentChirality
        return inherent == .unspecified ? nil : inherent
    }

    @available(visionOS 26.0, *)
    private func makeControllerState(from anchor: AccessoryAnchor, isLeftHand: Bool) -> VisionControllerState {
        let transform = anchor.originFromAnchorTransform
        let position = transform.translation
        // Apply the target profile's grip-convention correction (identity until tuned on-device).
        let orientation = simd_normalize(
            simd_quatf(transform.rotationMatrix)
            * SpatialControllerSupport.gripCorrection(isLeftHand: isLeftHand))

        // PSVR2 Sense buttons/trigger/grip/thumbstick, mapped to Meta Touch (Quest 2) per hand.
        var input = SpatialControllerSupport.Input()
        if case let .device(device) = anchor.accessory.source,
           let controller = device as? GCController {
            input = SpatialControllerSupport.input(from: controller, isLeftHand: isLeftHand)
        }

        return VisionControllerState(
            position: position,
            orientation: orientation,
            buttonState: input.buttons,
            trigger: input.trigger,
            grip: input.grip,
            thumbstick: input.thumbstick
        )
    }
}

private extension simd_float4x4 {
    var translation: SIMD3<Float> {
        SIMD3<Float>(columns.3.x, columns.3.y, columns.3.z)
    }

    var rotationMatrix: simd_float3x3 {
        simd_float3x3(
            SIMD3<Float>(columns.0.x, columns.0.y, columns.0.z),
            SIMD3<Float>(columns.1.x, columns.1.y, columns.1.z),
            SIMD3<Float>(columns.2.x, columns.2.y, columns.2.z)
        )
    }
}

private extension SIMD3 where Scalar == Float {
    var allFinite: Bool {
        x.isFinite && y.isFinite && z.isFinite
    }
}
