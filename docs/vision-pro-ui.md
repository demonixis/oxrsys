# Vision Pro UI And UX

## Product Contract

The visionOS client starts in a compact SwiftUI control window and enters a native immersive stream.
It must always provide a visible route to connect, enter immersion, return to the menu, or disconnect;
no transition may leave the user in an empty state.

## Current Flow

1. Search for a runtime on the local network.
2. Review the discovered server and connect explicitly, or use automatic immersive entry.
3. Enter the immersive space when streaming is ready.
4. Hide the control window by default while immersed, or keep it visible through the user setting.
5. Restore the control window when immersion closes, keeping re-entry and disconnect actions
   available while the connection remains active.

Connection state is surfaced as disconnected, discovering, connecting, streaming, or lost. A
user-initiated disconnect must not be treated as a network failure.

## Settings

Keep viewer settings small and platform-appropriate:

- automatic immersive entry
- keep control window visible while immersed
- visible hands / upper-limb visibility
- hand-gesture controller emulation and gamepad-assisted compatibility mode

Runtime-owned bitrate, codec, render-device, and server streaming controls remain in macOS Home.

## Interaction And Tracking

The immersive client returns head pose, per-eye FOV/IPD, hand joints, and tracked spatial-controller
data when visionOS exposes them. Controller emulation maps supported hand gestures, and optionally
gamepad state, into the existing controller tracking packet without changing its wire layout.

Hands visibility is a presentation preference; it must not silently disable tracking data required
by controller emulation.

## Recovery And Diagnostics

- Preserve the last coherent menu/connection state when the immersive space is dismissed.
- On stream loss, stop presenting stale video and offer a clear disconnect/search route.
- Keep detailed diagnostics optional and out of the primary connection flow.
- Never expose a UI setting before the runtime/client capability behind it is functional.

## Physical Acceptance

On a physical Vision Pro, verify discovery, explicit and automatic connection, immersive entry,
window hiding/restoration, re-entry, disconnect, stream loss, hand visibility, gesture emulation,
tracked accessory controllers, and app relaunch. Simulator success is only a compile and basic UI
gate.
