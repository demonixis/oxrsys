# Vision OS

## Scope

This page covers the native visionOS viewer in `clients/oxrsys-visionos/`.

## Current Role

The visionOS target is a first-pass native viewer that fits the Apple platform model rather than reusing the Android headset client unchanged. It currently provides:

- runtime discovery on the local network
- a minimal floating control window with a `Search` action
- UDP stream connection through the shared `OXRSysStreaming` package
- immersive stereo presentation through a native Metal compositor layer
- automatic immersive VR entry once the stream is connected
- 6DOF head-pose return while the immersive space is open
- hand-joint streaming through the shared 26-joint tracking payload
- first-pass accessory controller pose and input streaming when a tracked spatial controller is available

## Build

When using the Xcode UI, open `clients/OXRSys Clients.xcworkspace` rather than opening `OXRSys visionOS.xcodeproj`
while another client project is already open. `OXRSys visionOS` and `OXRSys Simulator` both depend on
the local `OXRSysStreaming` package, and Xcode can report `Missing package product 'OXRSysStreaming'`
if it has already loaded the package from another project window.

```bash
xcodebuild -project "clients/oxrsys-visionos/OXRSys visionOS.xcodeproj" \
  -scheme "OXRSys visionOS" \
  -configuration Debug \
  -destination 'generic/platform=visionOS Simulator' \
  build
```

For TestFlight upload, create a device archive:

```bash
xcodebuild -project "clients/oxrsys-visionos/OXRSys visionOS.xcodeproj" \
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
3. In the floating app window, press `Search`.
4. When the runtime is found, the viewer connects automatically.
5. Once the stream is active, the app dismisses the control window, opens its immersive space automatically, presents the stereo stream directly in VR, and starts returning headset pose.
6. On supported hardware, hand joints and tracked accessory controllers are folded into the same outbound tracking stream.

The current path is intentionally minimal: a floating launch window before connection, then full VR once streaming starts.

## Why It Is Separate

visionOS has different rendering, compositor, networking, and input constraints than Quest or the macOS/iOS simulator viewer. Keeping it separate avoids forcing the Apple headset path into assumptions from the Android client.

The current direction is to keep rendering and future passthrough work on the native Metal path. The viewer does not depend on Reality Composer content for its runtime video path.

## Current Limits

- The app is still an early viewer and not yet feature-complete.
- Hand tracking depends on ARKit authorization and on data actually exposed by the current simulator or device session.
- Accessory controller tracking is first-pass and only becomes active when visionOS exposes a tracked spatial accessory through Game Controller plus ARKit accessory tracking.
- The current control surface is intentionally minimal and does not yet expose reconnect or debug controls once the immersive stream is active.
