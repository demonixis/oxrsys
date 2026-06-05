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

Discovery announces the server and its stream settings. Video carries encoded frame fragments. Tracking carries headset and controller state back to the runtime. Control carries latency reports, keyframe requests, and haptics.

The Quest USB path uses ADB reverse TCP on localhost ports:

- Video TCP: `9944`
- Tracking TCP: `9945`
- Control TCP: `9946`

The runtime can run both transports in `auto` mode. `wifi` disables the TCP listeners. `usb_adb` disables WiFi discovery fallback.

TCP payloads are framed with `TcpRecordHeader`, which contains the record magic, protocol version, record type, and payload size. Current TCP record types are:

- `ServerAnnounce`
- `ClientConnect`
- `VideoNal`
- `RenderPose`
- `Tracking`
- `Control`
- `Disconnect`

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

`ClientConnect.maxBitrateMbps` is a client-side ceiling. A value of `0`
(`CLIENT_MAX_BITRATE_USE_SERVER_CONFIG`) means the client does not impose a
bitrate cap, so the runtime uses `streaming.bitrate_mbps` from its config. The
runtime accepts configured bitrates from `1` to `200` Mbps.

## Video Stream

UDP video packets use `VideoPacketHeader` followed by up to `1400` bytes of payload. The header includes:

- frame index
- packet index and total packet count
- payload size
- flags
- codec
- presentation timestamp

Current codec identifiers:

- `H265`
- `H264`
- `AV1`

USB TCP video sends complete encoded NAL units as `VideoNal` records. It does not use UDP fragmentation, FEC, or NACK recovery.

The runtime currently targets low-latency headset streaming. The queue is latest-frame-only rather than fully reliable.

The current stream also includes two recovery and timing helpers:

- `VIDEO_FLAG_FEC` marks XOR parity packets. One parity packet is sent per `FEC_GROUP_SIZE` data packets and can recover one lost data packet in that group.
- `VIDEO_FLAG_RENDER_POSE` marks metadata packets that carry the server render pose for a frame. These packets are not video data. Headset clients must match them to the decoded frame by presentation timestamp before submitting projection layers so compositor reprojection uses the pose that rendered that exact frame.

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

Latency reports allow the runtime to keep prediction bounded. Keyframe requests let the client recover after packet loss or decode stalls. Haptics are sent from the runtime to the client.

`NackRequest` lets a UDP client ask the runtime to retransmit specific recently sent video packets. It is a short-window recovery mechanism, not a guarantee of full stream reliability. USB TCP clients do not send NACKs.

## Session Lifecycle

The expected lifecycle is:

1. The runtime announces itself over UDP, or a USB TCP client connects to control port `9946` and receives `ServerAnnounce`.
2. A client connects and advertises capabilities.
3. The runtime starts video and tracking exchange.
4. The client sends latency feedback and keyframe requests while streaming is active.
5. Either side can disconnect and return to idle. USB TCP shutdown uses a best-effort `Disconnect` record on the control channel before sockets are closed; clients also treat closed video/control sockets as a connection loss and resume discovery/retry.

## Compatibility

The protocol is still internal and may evolve with the runtime and client implementations. When updating the wire format, keep `common/protocol/include/oxrsys/protocol/Protocol.h`, the runtime, the Android client, and this document aligned in the same change.
