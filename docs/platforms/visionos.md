# visionOS

## Scope

This page covers the native Vision Pro viewer in `clients/visionos/`. The application requires
visionOS 26 or later because its compositor path uses visionOS 26 APIs. The reusable
`OXRSysStreaming` package retains a visionOS 1 minimum, but that lower package minimum does not apply
to the app target.

## Current Role

The visionOS target is a first-pass native viewer that fits the Apple platform model rather than reusing the Android headset client unchanged. It currently provides:

- runtime discovery on the local network
- a compact floating control window with explicit server search, connect, immersive entry, disconnect, and session toggles
- UDP stream connection through the shared `OXRSysStreaming` package
- negotiated H.264/H.265 stream decoding through the shared VideoToolbox decoder, including 10-bit HEVC Main10 output, with H.265 kept as the preferred codec
- limited-range BT.709 YCbCr conversion with bit-depth-specific 8-bit and 10-bit normalization for correct SDR black levels and color balance
- immersive stereo presentation through a native Metal compositor layer, on a single shared ARKit world-tracking session with a depth-backed drawable so the compositor can reproject
- bounded world-space reprojection of each streamed frame from the runtime's per-frame render pose
  into the live head pose, with exact rotation and translation against the shared 2 m reprojection
  plane
- optional automatic immersive VR entry once the stream is connected, with the control window hidden by default while immersed and restored when the immersive space is dismissed
- 6DOF head-pose return while the immersive space is open, including the device's real per-eye FOV and IPD so the runtime renders a matching frustum
- hand-joint streaming through the shared 26-joint tracking payload, plus a user toggle for visible hands in the immersive space
- first-pass accessory controller pose and input streaming when a tracked spatial controller is available

## Build

When using the Xcode UI, open `clients/OXRSys Clients.xcworkspace` rather than opening `OXRSys visionOS.xcodeproj`
while another client project is already open. `OXRSys visionOS` and `OXRSys Simulator` both depend on
the local `OXRSysStreaming` package, and Xcode can report `Missing package product 'OXRSysStreaming'`
if it has already loaded the package from another project window.

```bash
xcodebuild -project "clients/visionos/OXRSys visionOS.xcodeproj" \
  -scheme "OXRSys visionOS" \
  -configuration Debug \
  -destination 'generic/platform=visionOS Simulator' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

Also compile the physical-device destination without signing:

```bash
xcodebuild -project "clients/visionos/OXRSys visionOS.xcodeproj" \
  -scheme "OXRSys visionOS" \
  -configuration Debug \
  -destination 'generic/platform=visionOS' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

Both commands prove compilation only. A signed physical Vision Pro build remains required for
network discovery, immersive presentation, tracking, decode, and latency qualification.

## Local Network Discovery

`Find Server` listens for the runtime's `ServerAnnounce` over IPv4 UDP broadcast. Apple requires the
restricted
[`com.apple.developer.networking.multicast`](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.networking.multicast)
entitlement for multicast or broadcast UDP on visionOS. Before signing the device build, the signing
team must obtain that capability from Apple, add it to the app's provisioning profile, and include
the matching entitlement in the app signature.

When that entitlement is unavailable, enter the Mac's IPv4 address or hostname and use the direct
`Connect` action. The client sends a bounded unicast discovery request to UDP `9946`; a broadcasting
runtime answers with its real, complete `ServerAnnounce`, after which the normal connection
handshake continues. This direct path does not require the multicast entitlement, but
`NSLocalNetworkUsageDescription` and user-granted Local Network access are still required on a
physical Vision Pro. The last valid direct host is retained as a convenience.

Do not infer physical discovery support from the Simulator or unsigned generic-device build. Replay
both automatic broadcast discovery on an entitled/provisioned Vision Pro and direct unicast
discovery with Local Network access as part of release qualification.

For TestFlight upload, create a device archive:

```bash
xcodebuild -project "clients/visionos/OXRSys visionOS.xcodeproj" \
  -scheme "OXRSys visionOS" \
  -configuration Release \
  -destination 'generic/platform=visionOS' \
  -archivePath /tmp/VisionPlayer.xcarchive \
  archive
```

The target is visionOS-only, so macOS `LSApplicationCategoryType` and App Sandbox entitlements do not
apply.

## Workflow

1. Launch the runtime on macOS.
2. Build and run `OXRSys visionOS` on visionOS Simulator or device.
3. In the floating app window, press `Find Server`, or enter the Mac's IPv4 address/hostname and
   press `Connect` for direct unicast discovery.
4. If `Auto-enter immersive` is enabled, the viewer connects to the first discovered runtime and opens its immersive space once the stream is active; otherwise, press `Connect` after discovery.
5. If the stream is active and the immersive space is not open, press `Enter Immersive`.
6. By default, the control window hides after immersive entry. Disable this with `Keep window in immersive` before connecting if you want the control window to remain open while immersed.
7. If the immersive space is dismissed, the control window reappears with `Enter Immersive` and `Disconnect` actions while the server connection remains active.
8. On supported hardware, hand joints and tracked accessory controllers are folded into the same outbound tracking stream. The `Show hands` toggle controls visionOS upper-limb visibility while immersed.

The current path is intentionally minimal: a floating launch window before connection, then a compact return menu around the immersive stream.

## Why It Is Separate

visionOS has different rendering, compositor, networking, and input constraints than Quest or the macOS/iOS simulator viewer. Keeping it separate avoids forcing the Apple headset path into assumptions from the Android client.

The current direction is to keep rendering and future passthrough work on the native Metal path. The viewer does not depend on Reality Composer content for its runtime video path.

## Current Limits

- The app is still an early viewer and not yet feature-complete.
- Hand tracking depends on ARKit authorization and on data actually exposed by the current simulator or device session.
- Accessory controller tracking is first-pass and only becomes active when visionOS exposes a tracked spatial accessory through Game Controller plus ARKit accessory tracking.
- The current control surface is intentionally minimal. It exposes disconnect and immersive
  re-entry controls, but detailed network and decoder diagnostics remain log-only.
