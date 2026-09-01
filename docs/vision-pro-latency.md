# Vision Pro Latency And Reprojection

## Current Pipeline

The visionOS client uses the shared decoder at
`clients/shared/OXRSysStreaming/Sources/OXRSysStreaming/VideoDecoder.swift` and presents through a
native CompositorServices renderer.

The latency-first decode contract is:

- set `kVTDecompressionPropertyKey_RealTime`
- enable, but never require, hardware-accelerated decode
- never combine real-time decode with `MaximizePowerEfficiency`
- scan Annex B start codes in place rather than copying the complete frame first
- after a decode failure, discard dependent frames and request a keyframe until a valid IDR/IRAP
  arrives
- retain the H.265 ten-bit-to-eight-bit output fallback when session creation rejects ten-bit output

The renderer uses two in-flight buffers. Increasing that count can add a compositor frame of
latency and requires an isolated measurement before adoption.

## Pose Matching And Timewarp

Every displayed frame is matched to the render pose carried with that encoded frame. The client
reprojects it from the server render pose into the latest ARKit head pose at presentation time.
Current timewarp includes rotation, translation against the bounded shared reprojection plane, and
the per-eye IPD lever-arm correction.

The client reports:

- locally measured head linear and angular velocity
- decoder, renderer pickup, in-flight, and compositor latency
- displayed-frame age and matched render-pose data

These values feed bounded server prediction. Missing or implausible pose data must fail closed rather
than applying an unbounded warp.

## Color, Foveation, And Sharpening

Latency changes must preserve visual contracts:

- BT.709 SDR limited-range conversion with exact 8-bit and 10-bit ranges
- the closed-form visionOS foveated inverse as the exact inverse of the server AADT transform
- display-space contrast-adaptive sharpening controlled by the server announcement
- matching per-eye FOV and IPD from the active visionOS drawable/ARKit state

Re-run fp32 round-trip tests whenever the foveation math changes.

## Physical Verification

Simulator compilation does not qualify this path. On a physical Vision Pro:

1. Stream the same controlled scene before and after the change.
2. Confirm hardware decode status when diagnosing decoder regressions.
3. Record server pipeline, decode, compositor, displayed-frame-age, and prediction telemetry.
4. Replay slow and fast yaw, pitch, vertical translation, lateral translation, and reconnect.
5. Verify no black-level, eye-projection, Main10, foveation, or sharpening regression.

Use controlled A-B-A measurements for latency changes. Do not present source-level optimizations or
FPS alone as proof of lower motion-to-photon latency.
