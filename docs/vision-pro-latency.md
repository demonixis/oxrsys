# Vision Pro — Latency & Jitter

**Branch:** `develop-avp` (latency track)

Goal of this track: minimize motion-to-photon latency and kill reprojection
jitter for the streamed visionOS client. Visual correctness (black screen, eye
projection, BT.709 color, Main10) is already handled; this track is about
*timing and stability*.

Work is staged. This document currently covers **item 1 — the decode pipeline**
(implemented) and lists the remaining items as a roadmap. Background research and
provenance: `docs/research/visionos-client-improvements.md` §2 on the
`optimization/vision-pro-latency` branch.

---

## 1. Low-latency decode pipeline — IMPLEMENTED

File: `clients/Apple/common/OXRSysStreaming/Sources/OXRSysStreaming/VideoDecoder.swift`

Three changes, all in the decode hot path, all low-risk:

### 1a. Real-time decode hint
`kVTDecompressionPropertyKey_RealTime = true` is set on every decompression
session right after creation. This tells VideoToolbox that latency matters more
than throughput, so it does not batch or hold frames.

- **Must never** be combined with `kVTDecompressionPropertyKey_MaximizePowerEfficiency`
  — combining the two is undefined behavior. We do not set the power key anywhere;
  keep it that way.

### 1b. Hardware-decoder preference
The decoder specification now passes
`kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder = true`, so a
real-time stream is never paced by a software decoder.

- We use **Enable**, not **Require**. Hardware HEVC/H.264 decode is always present
  on Apple silicon (all Apple Vision Pro, modern Macs), so this is effectively
  always honored — but `Enable` still permits a fallback rather than failing
  session creation outright on any configuration that lacks it. This composes
  safely with the existing 10-bit→8-bit output-format fallback loop.

### 1c. In-place NAL splitting (drops a per-frame whole-buffer copy)
`splitNalUnits` previously did `[UInt8](data)` — a full heap copy of every frame —
then sliced that array into per-NAL `Data`. It now scans Annex-B start codes
directly over the frame's bytes via `withUnsafeBytes`, and copies each NAL out
exactly once into an owned, 0-based `Data`.

- Owned + 0-based is deliberate: callers index `nal[0]` for the NAL type, and
  parameter sets (VPS/SPS/PPS) are *retained* across frames — both would be unsafe
  with a storage-sharing slice or a raw pointer into the transient receive buffer.
- Net effect: removes one full-frame copy and the per-byte bounds checking on the
  decode path per frame. Not literally zero-copy (the NAL payload is still copied
  once, which is required), but it removes the dominant redundant copy §2.2 flagged.

### Verification (do on device)
- Console: `[VideoDecoder/H.265] Decoder session created - WxH (10-bit|8-bit)`.
- Confirm hardware decode is actually in use by reading
  `kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder` off the
  session (diagnostic; not wired up yet — add if HW usage is ever in doubt).
- Measure decode→display latency against the pre-change baseline (LatencyReporter /
  JitterDiag). The RealTime hint should shave queued-frame latency; the NAL change
  should reduce per-frame CPU at high bitrate/refresh.

---

## Deferred — needs an isolated experiment (do NOT bundle with the above)

- **Drop `_EnableAsynchronousDecompression`.** For an all-P (no B-frame) stream,
  synchronous one-in/one-out decode can lower and de-jitter latency — but async can
  help throughput and dropping it risks frame starvation. §2.1 says *measure first*.
  Left ON for now. Requires confirming the server encodes zero B-frames.
- **maxBuffersInFlight 3 → 2** (`ImmersiveRenderer`): lower latency, possible frame
  starvation. Isolated A/B.
- **Fixed 1.5 m compositor depth**: may improve translation handling but distorts
  content at other depths. Isolated.
- **Adaptive jitter buffer**: smooths unstable Wi-Fi at the cost of intentional
  added latency. Isolated.

---

## Roadmap — remaining items (later)

Recommended order, per the review of `optimization/vision-pro-latency`:

- [x] 1. Low-latency VideoToolbox decode, HW-decoder preference, in-place NAL split
- [ ] 2. Decoder corruption recovery: after packet loss, discard dependent frames
      until an IDR/IRAP (HEVC NAL types 16–23) arrives (see §2.3 keyframe-by-NAL-type)
- [ ] 3. Live ARKit provider lookup + last-valid-anchor fallback during reconnects
- [ ] 4. Locally measured linear/angular head velocity for better server pose prediction
- [ ] 5. Measured decode-to-photon latency for auto-calibrated prediction
- [ ] 6. visionOS foveated-stream decoding
- [ ] 7. Unified resolution presets + corrected foveation layouts (touches Quest/ABR)
- [ ] 8. Optional contrast-adaptive sharpening
- [ ] 9. Controller emulation via hand gestures / hands + gamepad (separate from latency)

Do **not** merge `optimization/vision-pro-latency` wholesale — it predates the
current codec generalization and couples protocol, runtime, UI, and rendering
experiments. Port items individually, as done for item 1.
