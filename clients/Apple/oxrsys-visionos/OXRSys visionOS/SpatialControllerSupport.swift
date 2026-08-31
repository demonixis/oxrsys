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
        func pressed(_ name: String) -> Bool { profile.buttons[name]?.isPressed ?? false }
        func value(_ name: String) -> Float { profile.buttons[name]?.value ?? 0 }

        var input = Input()
        input.trigger = value(Sense.trigger)
        input.grip = max(value(Sense.grip), pressed(Sense.grip) ? 1 : 0)
        input.thumbstick = thumbstick(profile, isLeftHand: isLeftHand)

        let menu = Sense.menuNames.contains(where: pressed)

        switch target {
        case .metaTouch:
            if isLeftHand {
                if pressed(Sense.buttonA) { input.buttons |= ButtonFlags.x }
                if pressed(Sense.buttonB) { input.buttons |= ButtonFlags.y }
            } else {
                if pressed(Sense.buttonA) { input.buttons |= ButtonFlags.a }
                if pressed(Sense.buttonB) { input.buttons |= ButtonFlags.b }
            }
            if menu { input.buttons |= ButtonFlags.menu }
        }
        return input
    }

    /// The thumbstick, which `GCInput` models as a direction-pad element (not two axes) on the
    /// Sense. Falls back to a prefixed dpad and finally to axis elements for controllers that
    /// publish those instead.
    private static func thumbstick(_ profile: GCPhysicalInputProfile, isLeftHand: Bool) -> SIMD2<Float> {
        for name in [Sense.thumbstick, isLeftHand ? "Left Thumbstick" : "Right Thumbstick"] {
            if let dpad = profile.dpads[name] {
                return SIMD2(dpad.xAxis.value, dpad.yAxis.value)
            }
        }
        let x = profile.axes["Thumbstick X Axis"]?.value ?? 0
        let y = profile.axes["Thumbstick Y Axis"]?.value ?? 0
        return SIMD2(x, y)
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
