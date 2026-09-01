// SPDX-License-Identifier: MPL-2.0

import GameController
import OXRSysStreaming
import simd

/// Maps a physically-tracked spatial controller (currently the PSVR2 Sense) onto a target VR
/// controller profile, so the streamed game sees controllers it already understands.
///
/// Kept separate from the tracking plumbing so new *targets* (standard Khronos XR, Oculus) and new
/// *devices* stay additive: each is a small self-contained case here rather than a change threaded
/// through `VisionTrackingManager`. For now the only target is Meta Touch (Quest 2) — the simplest
/// and most widely bound profile — which is what the runtime already advertises for streaming.
enum SpatialControllerSupport {

    /// The controller layout presented to the streamed game. `.standardXR` / `.oculus` slot in
    /// here later without touching the call sites.
    enum Target {
        case metaTouch
    }

    /// Mapped controller input for one hand, in the units the tracking packet carries.
    struct Input {
        var buttons: UInt32 = 0
        var trigger: Float = 0
        var grip: Float = 0
        var thumbstick: SIMD2<Float> = .zero
    }

    /// PSVR2 Sense element names, read from `GCPhysicalInputProfile`. Both Sense controllers publish
    /// the SAME unprefixed names — the hand is the device, not the element — so the mapping is keyed
    /// on the hand rather than on "Left …"/"Right …" strings. A name the hardware does not publish
    /// is indistinguishable from an unpressed button (`nil` → `?? false` → released), so these are
    /// the names an actual Sense pair was observed to report.
    private enum Sense {
        static let buttonA = "Button A"
        static let buttonB = "Button B"
        static let trigger = "Trigger"
        static let grip = "Grip"
        static let thumbstick = "Thumbstick"            // a direction pad, not two axes
        static let menuNames = ["Button Menu", "Button Share", "Button Options"]
    }

    /// Read the controller's current inputs and map them to `target` for the given hand.
    ///
    /// Per-hand because Touch splits its face buttons across the pair: the Sense names both face
    /// buttons A/B on either controller, while Touch calls the left pair X/Y and the right pair A/B.
    /// The packet ORs the two hands together, and the Touch interaction profile only binds `menu` on
    /// one hand, so publishing menu on both never cross-fires.
    static func input(from controller: GCController,
                      isLeftHand: Bool,
                      target: Target = .metaTouch) -> Input {
        let profile = controller.physicalInputProfile
        logElementNamesOnce(controller, profile)

        func pressed(_ name: String) -> Bool { profile.buttons[name]?.isPressed ?? false }
        func value(_ name: String) -> Float { profile.buttons[name]?.value ?? 0 }
        // A name the device does not publish reads as "released", so a wrong name is silently
        // indistinguishable from an unpressed button. Fall back to the strongly-typed
        // extendedGamepad view (the same API the gamepad compatibility path uses) whenever the
        // expected named element is absent, so face buttons still work if the names differ.
        let pad = controller.extendedGamepad
        func namedOrPad(_ name: String, _ fallback: GCControllerButtonInput?) -> Bool {
            if let button = profile.buttons[name] { return button.isPressed }
            return fallback?.isPressed ?? false
        }
        func namedOrPadValue(_ name: String, _ fallback: GCControllerButtonInput?) -> Float {
            if let button = profile.buttons[name] { return button.value }
            return fallback?.value ?? 0
        }

        var input = Input()
        // The Sense pair exposes one trigger/grip per device, so prefer the hand's own side and
        // accept either side from the generic view (a single-hand device may publish either).
        input.trigger = namedOrPadValue(Sense.trigger,
                                        isLeftHand ? pad?.leftTrigger : pad?.rightTrigger)
        if input.trigger == 0, let pad {
            input.trigger = max(pad.leftTrigger.value, pad.rightTrigger.value)
        }
        let gripValue = namedOrPadValue(Sense.grip,
                                        isLeftHand ? pad?.leftShoulder : pad?.rightShoulder)
        input.grip = max(gripValue, pressed(Sense.grip) ? 1 : 0)
        input.thumbstick = thumbstick(profile, isLeftHand: isLeftHand, pad: pad)

        let menu = Sense.menuNames.contains(where: pressed)
            || pad?.buttonMenu.isPressed == true
            || pad?.buttonOptions?.isPressed == true

        switch target {
        case .metaTouch:
            // Touch splits the face buttons across the pair: left is X/Y, right is A/B. Each Sense
            // publishes its own two face buttons, so read the device's pair and place them on the
            // side this hand represents.
            let lower = namedOrPad(Sense.buttonA, pad?.buttonA) || namedOrPad(Sense.buttonA, pad?.buttonX)
            let upper = namedOrPad(Sense.buttonB, pad?.buttonB) || namedOrPad(Sense.buttonB, pad?.buttonY)
            if isLeftHand {
                if lower { input.buttons |= ButtonFlags.x }
                if upper { input.buttons |= ButtonFlags.y }
            } else {
                if lower { input.buttons |= ButtonFlags.a }
                if upper { input.buttons |= ButtonFlags.b }
            }
            if menu { input.buttons |= ButtonFlags.menu }
        }
        return input
    }

    // One-shot per controller: dump every element the device actually publishes. A wrong element
    // name is invisible at runtime (it just reads as released), so this is the ground truth needed
    // to map a new spatial controller correctly.
    nonisolated(unsafe) private static var loggedElementNames = Set<String>()

    private static func logElementNamesOnce(_ controller: GCController,
                                            _ profile: GCPhysicalInputProfile) {
        let key = controller.vendorName ?? "unknown"
        guard !loggedElementNames.contains(key) else { return }
        loggedElementNames.insert(key)
        print("[SpatialDiag] '\(key)' extendedGamepad=\(controller.extendedGamepad != nil)")
        print("[SpatialDiag]   buttons: \(profile.buttons.keys.sorted())")
        print("[SpatialDiag]   axes:    \(profile.axes.keys.sorted())")
        print("[SpatialDiag]   dpads:   \(profile.dpads.keys.sorted())")
    }

    /// The thumbstick, which `GCInput` models as a direction-pad element (not two axes) on the
    /// Sense. Falls back to a prefixed dpad and finally to axis elements for controllers that
    /// publish those instead.
    private static func thumbstick(_ profile: GCPhysicalInputProfile,
                                   isLeftHand: Bool,
                                   pad: GCExtendedGamepad?) -> SIMD2<Float> {
        for name in [Sense.thumbstick, isLeftHand ? "Left Thumbstick" : "Right Thumbstick"] {
            if let dpad = profile.dpads[name] {
                return SIMD2(dpad.xAxis.value, dpad.yAxis.value)
            }
        }
        let x = profile.axes["Thumbstick X Axis"]?.value ?? 0
        let y = profile.axes["Thumbstick Y Axis"]?.value ?? 0
        if x != 0 || y != 0 { return SIMD2(x, y) }
        // Same reasoning as the buttons: fall back to the generic view when the named element is
        // absent, taking whichever stick this single-hand device actually drives.
        guard let pad else { return .zero }
        let side = isLeftHand ? pad.leftThumbstick : pad.rightThumbstick
        if side.xAxis.value != 0 || side.yAxis.value != 0 {
            return SIMD2(side.xAxis.value, side.yAxis.value)
        }
        let other = isLeftHand ? pad.rightThumbstick : pad.leftThumbstick
        return SIMD2(other.xAxis.value, other.yAxis.value)
    }

    /// Orientation correction applied to the tracked accessory pose to match the target profile's
    /// grip convention. Identity for now: the PSVR2's tracked frame and the Touch grip frame differ,
    /// but the exact offset is only knowable on-device, so this is the single tuning point — return
    /// a fixed rotation here (mirroring the X term per hand if the two controllers need opposite
    /// tilts) once the on-device pose is observed, the same way the Quest client's grip/aim was tuned.
    static func gripCorrection(isLeftHand: Bool, target: Target = .metaTouch) -> simd_quatf {
        simd_quatf(ix: 0, iy: 0, iz: 0, r: 1)
    }
}
