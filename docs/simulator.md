# Simulator

## Purpose

The OXRSys simulator is the software streaming client used for desktop development and for the iOS
Cardboard-style viewer. It exercises discovery, video decode, frame presentation, tracking return,
FOV metadata, keyframe recovery, and runtime telemetry without duplicating the client stack.

The macOS variant requires macOS 14 or later. The iOS simulator/Cardboard variant requires iOS 17
or later.

## Layout

- `clients/shared/OXRSysSimulator/`: shared SwiftUI simulator model, views, and tracking integration
- `clients/shared/OXRSysStreaming/`: discovery, transport, protocol, VideoToolbox decode, and renderer
- `clients/simulator/`: standalone macOS/iOS app target
- `clients/home/`: embeds the same simulator package in the optional Developer workflow

The standalone app and Home integration must remain behaviorally aligned because they use the same
package rather than separate implementations.

## macOS Simulator

The macOS variant uses a SwiftUI interface with platform-scoped Metal and input adapters. It can
discover or connect to OXRSys, present the decoded stereo stream, and send mouse-driven synthetic
head tracking. The simulator owns its vertical FOV and sends per-eye FOV metadata in tracking
packets.

Build it with:

```bash
xcodebuild -project "clients/simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" \
  -configuration Debug \
  -destination 'platform=macOS' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

## iOS Cardboard Variant

The iOS target uses the same streaming and presentation path with a Cardboard-style side-by-side
view. ARKit supplies device tracking on supported hardware. iOS-only tracking sources live under the
package's `Platform/iOS` source boundary.

Compile the simulator and generic-device variants with:

```bash
xcodebuild -project "clients/simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" -configuration Debug \
  -destination 'generic/platform=iOS Simulator' \
  CODE_SIGNING_ALLOWED=NO build

xcodebuild -project "clients/simulator/OXRSys Simulator.xcodeproj" \
  -scheme "OXRSys Simulator" -configuration Debug \
  -destination 'generic/platform=iOS' \
  CODE_SIGNING_ALLOWED=NO build
```

The iOS Simulator cannot qualify physical ARKit motion or Cardboard comfort. Use a signed build on a
physical iPhone for projection, orientation, touch, reconnect, thermal, and tracking validation.

## Modes And Controls

- `Simulator` presents the received stereo surface with synthetic or ARKit tracking.
- `StereoView` is the phone-oriented side-by-side/Cardboard presentation.
- Discovery and explicit connection use the shared streaming package.
- Reset returns the synthetic or ARKit tracking origin to the client-defined neutral pose.
- Vertical FOV belongs to the client and is sent to the runtime; it is not a server render setting.

Keep decoded-frame presentation matched to render-pose metadata. Recovery requests should prefer a
clean freeze and keyframe request over presenting corrupted frames.

## Verification

For simulator changes, separately report:

- Swift package build/test
- macOS arm64 build
- macOS x86_64 build
- iOS Simulator compile
- generic iOS device compile
- live macOS stream and input behavior
- physical iPhone Cardboard and ARKit behavior

The compile lanes do not replace the last two interactive checks.
