# Roadmap

## Runtime correctness

- [ ] Support quad layers and multiple projection layers.
- [ ] Clear the remote image when an application submits an empty frame.
- [ ] Align poses and velocities to the requested `XrTime` using the same tracking samples.
- [ ] Invalidate head and controller poses when tracking becomes stale or unavailable.
- [ ] Forward headset recenter events, change timestamps, and reference-space origin changes.
- [ ] Transform hand joints into the requested `baseSpace` and preserve joint orientation and validity.

## Client reliability

- [ ] Synchronize iOS hand tracking and recenter state.
- [ ] Apply the same recenter transform to head and hand poses on iOS.
- [ ] Isolate connection callbacks and protect socket lifetimes during sends, disconnects, and restarts.
- [ ] Complete macOS Simulator privacy permission descriptions.

## Missing features

- [ ] Deliver and cancel headset haptic commands.
- [ ] Implement headset audio capture, transport, and playback.
- [ ] Implement spatial anchors, scene queries, and persistence.
- [ ] Implement environment-depth and scene-mesh occlusion.

## Qualification

- [ ] Complete visionOS device and Simulator builds with the required Xcode platform components.
- [ ] Rerun OpenXR CTS and qualify interactive behavior.
- [ ] Validate Metal and MoltenVK streaming on physical Apple Silicon and Intel Macs.
- [ ] Validate Quest, Pico, Vision Pro, and iPhone streaming, including measured velocities, available STAGE bounds, recentering, tracking loss, and supported Wi-Fi/USB transports.
