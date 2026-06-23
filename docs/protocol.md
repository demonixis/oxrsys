# Protocol

## Scope

This document describes the internal streaming protocol shared by the macOS runtime and headset clients. The authoritative wire layout lives in `common/protocol/include/oxrsys/protocol/Protocol.h`.

## Goals

- Keep motion-to-photon latency bounded.
- Prefer fresh state over full reliability.
- Keep the protocol simple enough to iterate on during runtime development.

## Transport Model

The WiFi transport uses UDP with dedicated ports:

- Discovery: `9943`
- Video: `9944`
- Tracking: `9945`
- Control: `9946`
- Audio: `9947` (reserved for headset speaker audio; not advertised until an audio stream is active)
- Spatial: `9948` (reserved reliable channel for anchors, scene capture, meshes, and larger async spatial results)

Discovery announces the server and its stream settings. Video carries encoded frame fragments. Tracking carries headset and controller state back to the runtime. Control carries latency reports, stream reconfiguration, keyframe requests, and haptics. Spatial is a reliable side channel for future spatial entity and scene data.

The Quest USB path uses ADB reverse TCP on localhost ports:

- Video TCP: `9944`
- Tracking TCP: `9945`
- Control TCP: `9946`
- Spatial TCP: `9948`

The runtime can run both transports in `auto` mode. `wifi` disables the TCP listeners. `usb_adb` disables WiFi discovery fallback.

TCP payloads are framed with `TcpRecordHeader`, which contains the record magic, protocol version, record type, and payload size. Current TCP record types are:

- `ServerAnnounce`
- `ClientConnect`
- `VideoNal`
- `RenderPose`
- `Tracking`
- `Control`
- `Disconnect`
- `Audio`
- `Spatial`

## Discovery

The runtime broadcasts `ServerAnnounce` messages. Clients answer with `ClientConnect`.

The handshake exposes:

- protocol version
- advertised ports
- render and encoded resolution
- refresh rate
- server and device names; Android clients send the OpenXR `systemName` in
  `ClientConnect.deviceName`
- preferred codec and bitrate limits
- server feature flags for foveated encoding, client foveation override, client upscaling, stream
  reconfiguration, passthrough, occlusion, spatial/scene support, and reserved headset audio
- client capability flags for foveated encoding, client foveation, client upscaling, stream
  reconfiguration, passthrough, occlusion, spatial/scene support, and audio output
- foveated encoding preset and aligned AADT parameters
- client foveation override preset and client upscaling mode
- client reprojection mode
- audio port and sample rate fields reserved for headset speaker audio

`ServerAnnounce` is versioned as a 92-byte v1.0 base followed by v1.1 and v1.2 trailing fields.
`ClientConnect` is versioned as an 80-byte v1.0 base followed by v1.1 trailing fields. Receivers
accept either the base size or the full struct size and zero-initialize missing trailing fields.

`ServerAnnounce.clientReprojectionMode` lets the runtime choose the Quest/PICO fallback behavior when
no newly decoded video frame is ready:

- `Off`: display only the available video path without stale-frame pose reprojection.
- `Pose`: reuse the last decoded texture for short gaps and submit the layer using the matched
  render pose when it is available.
- `PoseWarp`: allow the client to add a conservative GLES image-space orientation correction on top
  of `Pose`; clients must disable it on stale frames that are too old, missing pose data, large
  translation, recovery conditions, or repeated reuse.

`ClientConnect.maxBitrateMbps` is a client-side ceiling. A value of `0`
(`CLIENT_MAX_BITRATE_USE_SERVER_CONFIG`) means the client does not impose a
bitrate cap, so the runtime uses `streaming.bitrate_mbps` from its config. The
runtime accepts configured bitrates from `1` to `200` Mbps.

The runtime announces the configured preferred headset refresh rate. Current Home-supported values
are `60`, `72`, `80`, `90`, and `120` Hz. Quest clients request the announced value through
`XR_FB_display_refresh_rate` when available and report the active rate back in
`ClientConnect.refreshRateHz`; the runtime uses that reported value for encode cadence and pose
prediction.

Foveated encoding uses an ALVR-style axis-aligned distortion transform before video encode on
supported server paths. The announced presets currently map to:

- `Light`: center `0.60 x 0.55`, edge ratio `2 x 3`
- `Medium`: center `0.45 x 0.40`, edge ratio `4 x 5`
- `High`: center `0.35 x 0.32`, edge ratio `6 x 7`

The server aligns the active foveated area to encoder-friendly dimensions and announces the encoded
resolution. A client must advertise `CLIENT_CAPABILITY_FOVEATED_ENCODING` before the server applies
the AADT compression shader. Clients that do not advertise the capability can still receive the
reduced encoded resolution as a normal downscaled stream. Until the protocol carries a separate
foveated target size, the server enables AADT only when the shader target can match the announced
source dimensions coherently; otherwise it announces a normal stream.

## Video Stream

UDP video packets use `VideoPacketHeader` followed by up to `1400` bytes of payload. The header includes:

- frame index
- packet index and total packet count
- payload size
- flags
- codec
- FEC group final-packet payload size, only meaningful on `VIDEO_FLAG_FEC` packets
- presentation timestamp

Current codec identifiers:

- `H265`
- `H264`
- `AV1`

USB TCP video sends complete encoded NAL units as `VideoNal` records. It does not use UDP fragmentation, FEC, or NACK recovery.

The runtime currently targets low-latency headset streaming. Frame submission and encoded-video
dispatch are bounded and latest-frame-oriented rather than fully reliable; if the transport cannot
keep up, stale encoded frames may be dropped so socket backpressure does not block the encoder
callback or `Session::EndFrame()`.

The current stream also includes two recovery and timing helpers:

- `VIDEO_FLAG_FEC` marks XOR parity packets. One parity packet is sent per `FEC_GROUP_SIZE` data packets and can recover one lost data packet in that group. FEC packets also carry the payload size of that group's last data packet in the existing 24-byte header padding. Receivers use that size only when the recovered packet is the last packet of the group; other recovered packets remain `MAX_PACKET_PAYLOAD`.
- `VIDEO_FLAG_RENDER_POSE` marks metadata packets that carry the server render pose for a frame. These packets are not video data. Headset clients must match them to the decoded frame by presentation timestamp before submitting projection layers so compositor reprojection uses the pose that rendered that exact frame.
- `VIDEO_FLAG_ALPHA_BLEND` marks frames submitted by the app with `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` or a projection layer using `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`. Quest clients use this with server-enabled passthrough to reveal the passthrough underlay; the current stream does not carry a full alpha plane. If passthrough is active and no alpha flags have appeared in the stream, Quest clients may temporarily use the same black-key fallback for transparent-clear AR demos.

For passthrough, `SERVER_FEATURE_MIXED_REALITY_PASSTHROUGH` means the desktop runtime is configured
to allow app-requested passthrough. The headset still has to advertise
`CLIENT_CAPABILITY_MIXED_REALITY_PASSTHROUGH`, which the Android client sets only after its local
OpenXR runtime exposes `XR_FB_passthrough`, reports `supportsPassthrough`, and successfully creates
the passthrough objects. Runtime status reports `passthrough_ready` only when both sides are true.

## Tracking Stream

`TrackingPacket` carries the same payload over UDP tracking packets or TCP `Tracking` records:

- headset pose
- headset linear and angular velocity when the client can provide it
- left and right controller poses
- buttons, triggers, grips, and thumbsticks
- IPD and eye FOV overrides
- optional 26-joint hand tracking payloads for each hand

Hand presence is indicated by `TRACKING_FLAG_LEFT_HAND_ACTIVE` and `TRACKING_FLAG_RIGHT_HAND_ACTIVE`.
Controller pose presence is indicated independently by `TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE` and
`TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE`. If a controller flag is absent, the runtime treats that
controller as inactive and preserves the last valid pose instead of applying zeroed packet fields.

Velocity values are optional. Zero vectors mean the runtime should fall back to its bounded finite-difference prediction path.

## Control Channel

The control channel currently defines the following payloads over UDP or TCP `Control` records:

- `LatencyReport`
- `RequestKeyframe`
- `HapticsCommand`
- `NackRequest`
- `StreamConfigUpdate`
- `StreamConfigAck`

Latency reports allow the runtime to keep prediction bounded. The first 20 bytes are the historical
base report: receive-to-submit, decode, compositor, and total client latency. Newer clients append
displayed frame age, reprojected-frame count, stale-frame reuse count, render-pose fallback count,
and the active client reprojection mode. The runtime accepts the base report size for older clients
and treats missing trailing metrics as zero.

The runtime ABR controller consumes the latency report, displayed frame age, keyframe request
deltas, video-send drops, encoder drops, and reprojection pressure. `streaming.abr_mode = "bitrate"`
adjusts encoder bitrate only. `full` selects encoded-resolution targets when the client advertises
`CLIENT_CAPABILITY_STREAM_RECONFIGURE` and the active control path is reliable USB TCP: `quality`
and `balanced` use `resolution_scale`, `smooth` uses
`max(dynamic_resolution_min_scale, resolution_scale * 0.85)`, and `wifi_smooth` uses
`max(dynamic_resolution_min_scale, resolution_scale * 0.70)`. WiFi control remains best-effort in
this version, so WiFi clients fall back to bitrate/profile status without live decoder
reconfiguration. For USB TCP, the runtime sends `StreamConfigUpdate` outside `Session::EndFrame()`,
the client recreates its decoder on a client worker thread, replies with `StreamConfigAck` only after
the decoder is ready, and the runtime swaps encoders, clears queued video, and forces an IDR only
after the ack. Pending updates have bounded retry/timeout behavior; timeout disconnects the client
so encoder and decoder state recover through the normal reconnect path.

Protocol v1.2 extends `ServerAnnounce` with `spatialPort` and adds feature/capability flags for
stream reconfiguration, passthrough, depth occlusion, spatial entities, and scene capture. Spatial
OpenXR extensions are still advertised only when there is a coherent runtime
implementation or fallback for the selected mode.

Keyframe requests let the client recover after packet loss or decode stalls. Haptics are sent from
the runtime to the client.

`NackRequest` lets a UDP client ask the runtime to retransmit specific recently sent video packets. It is a short-window recovery mechanism, not a guarantee of full stream reliability. USB TCP clients do not send NACKs.

## Audio Stream

The protocol reserves stereo 48 kHz float PCM speaker audio over UDP `AUDIO_PORT` or TCP `Audio`
records. The wire structs are present so clients and frontends can parse the protocol safely, but the
runtime does not advertise `SERVER_FEATURE_HEADSET_AUDIO` until a real capture/playback path is
attached. Microphone input is out of scope for this speaker-only path.

## Session Lifecycle

The expected lifecycle is:

1. The runtime announces itself over UDP, or a USB TCP client connects to control port `9946` and receives `ServerAnnounce`.
2. A client connects and advertises capabilities.
3. The runtime starts video, tracking, and optional spatial-channel exchange.
4. The client sends latency feedback, stream config acknowledgements, and keyframe requests while streaming is active.
5. Either side can disconnect and return to idle. USB TCP shutdown uses a best-effort `Disconnect` record on the control channel before sockets are closed; clients also treat closed video/control sockets as a connection loss and resume discovery/retry.

## Compatibility

The protocol is still internal and may evolve with the runtime and client implementations. When updating the wire format, keep `common/protocol/include/oxrsys/protocol/Protocol.h`, the runtime, the Android client, and this document aligned in the same change.
