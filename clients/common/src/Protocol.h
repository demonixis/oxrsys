// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>

namespace oxr
{
namespace protocol
{

// ─── Network Ports ──────────────────────────────────────────────────────────

constexpr uint16_t DISCOVERY_PORT = 9943;
constexpr uint16_t VIDEO_PORT = 9944;
constexpr uint16_t TRACKING_PORT = 9945;
constexpr uint16_t CONTROL_PORT = 9946;
constexpr uint32_t HAND_JOINT_COUNT = 26;

enum class StreamingTransport : uint8_t
{
    Auto = 0,
    Wifi = 1,
    UsbAdb = 2,
};

// ─── TCP Framing (USB ADB reverse transport) ─────────────────────────────────

constexpr uint32_t TCP_RECORD_MAGIC = 0x4f585255; // "OXRU", little-endian on supported targets
constexpr uint16_t TCP_RECORD_VERSION = 1;
constexpr uint32_t TCP_MAX_RECORD_PAYLOAD = 16 * 1024 * 1024;

enum class TcpRecordType : uint16_t
{
    ServerAnnounce = 0x0001,
    ClientConnect = 0x0002,
    VideoNal = 0x0003,
    RenderPose = 0x0004,
    Tracking = 0x0005,
    Control = 0x0006,
    Disconnect = 0x0007,
};

struct TcpRecordHeader
{
    uint32_t magic = TCP_RECORD_MAGIC;
    uint16_t version = TCP_RECORD_VERSION;
    TcpRecordType type = TcpRecordType::Control;
    uint32_t payloadSize = 0;
};

struct TcpVideoNalHeader
{
    int64_t presentationTimeNs = 0;
    uint32_t frameIndex = 0;
    uint32_t payloadSize = 0;
    uint8_t flags = 0;
    uint8_t codec = 0;
    uint16_t reserved = 0;
    uint32_t reserved2 = 0;
};

struct TcpRenderPose
{
    int64_t presentationTimeNs = 0;
    uint32_t frameIndex = 0;
    uint32_t reserved = 0;
    float position[3] = {};
    float orientation[4] = {0, 0, 0, 1};
    uint32_t reserved2 = 0;
};

// ─── Discovery (UDP broadcast on DISCOVERY_PORT) ────────────────────────────

enum class MessageType : uint8_t
{
    ServerAnnounce = 0x01,
    ClientConnect = 0x02,
    ServerDisconnect = 0x03,
};

struct ServerAnnounce
{
    MessageType type = MessageType::ServerAnnounce;
    uint8_t versionMajor = 1;
    uint8_t versionMinor = 0;
    uint8_t reserved = 0;
    uint16_t videoPort = VIDEO_PORT;
    uint16_t trackingPort = TRACKING_PORT;
    uint32_t renderWidth;      // Stereo side-by-side width (2x per-eye)
    uint32_t renderHeight;     // Per-eye height
    uint32_t refreshRateHz;    // Target refresh rate (72, 90, 120)
    uint32_t encodedWidth;     // Actual H.265 encoded width (may be < renderWidth if scaled)
    uint32_t encodedHeight;    // Actual H.265 encoded height (may be < renderHeight if scaled)
    char serverName[64];       // Null-terminated UTF-8
};

struct ClientConnect
{
    MessageType type = MessageType::ClientConnect;
    uint8_t versionMajor = 1;
    uint8_t versionMinor = 0;
    uint8_t reserved = 0;
    uint32_t preferredCodec;   // See VideoCodec enum
    uint32_t maxBitrateMbps;
    uint32_t refreshRateHz;    // Actual client display refresh rate
    char deviceName[64];       // e.g. "Quest 3", "Pico 4"
};

// ─── Video Stream (Server → Client, UDP on VIDEO_PORT) ──────────────────────

enum class VideoCodec : uint32_t
{
    H265 = 0,
    H264 = 1,
    AV1 = 2,
};

struct VideoPacketHeader
{
    uint32_t frameIndex;
    uint16_t packetIndex;      // Within this frame
    uint16_t totalPackets;     // For this frame
    uint16_t payloadSize;
    uint8_t flags;             // See VideoFlags
    uint8_t codec;             // VideoCodec cast to u8
    int64_t presentationTimeNs; // Server-side timestamp
};

enum VideoFlags : uint8_t
{
    VIDEO_FLAG_KEYFRAME = 0x01,
    VIDEO_FLAG_END_OF_FRAME = 0x02,
    VIDEO_FLAG_LEFT_EYE = 0x04,
    VIDEO_FLAG_RIGHT_EYE = 0x08,
    VIDEO_FLAG_STEREO = 0x0C,  // Both eyes in one frame
    VIDEO_FLAG_FEC = 0x10,     // Forward Error Correction parity packet
    VIDEO_FLAG_RENDER_POSE = 0x20, // Payload contains the server's render pose for this frame
};

// FEC: 1 parity packet per group of N data packets (XOR-based recovery).
// Recovers up to 1 lost data packet per group. ~10% bandwidth overhead.
constexpr uint32_t FEC_GROUP_SIZE = 10;

// Max payload to fit in a single UDP packet within typical MTU (1500)
constexpr size_t MAX_PACKET_PAYLOAD = 1400;
constexpr size_t VIDEO_PACKET_SIZE = sizeof(VideoPacketHeader) + MAX_PACKET_PAYLOAD;

// ─── Tracking Stream (Client → Server, UDP on TRACKING_PORT) ────────────────

struct TrackingPacket
{
    int64_t timestampNs;       // Client monotonic clock
    uint32_t trackingFlags;    // See TrackingFlags

    // Head pose
    float headPosition[3];     // x, y, z in meters
    float headOrientation[4];  // quaternion (x, y, z, w)

    // Left controller
    float leftControllerPos[3];
    float leftControllerRot[4];

    // Right controller
    float rightControllerPos[3];
    float rightControllerRot[4];

    // Input state
    uint32_t buttonState;      // Bitfield (see ButtonFlags)
    float leftTrigger;
    float rightTrigger;
    float leftGrip;
    float rightGrip;

    // Thumbsticks
    float leftThumbstick[2];   // x, y
    float rightThumbstick[2];  // x, y

    // Eye data from headset
    float ipd;                 // Inter-pupillary distance in meters (0 = use default)
    float eyeFov[4];           // Left eye FOV: left, right, up, down (radians, 0 = use default)

    // Velocity from IMU (for improved server-side pose prediction)
    float headLinearVelocity[3];   // m/s (0 = not available)
    float headAngularVelocity[3];  // rad/s (0 = not available)

    // Optional hand tracking payload per joint: x, y, z, radius
    float leftHandJoints[HAND_JOINT_COUNT][4];
    float rightHandJoints[HAND_JOINT_COUNT][4];
};

enum ButtonFlags : uint32_t
{
    BUTTON_A = 0x0001,
    BUTTON_B = 0x0002,
    BUTTON_X = 0x0004,
    BUTTON_Y = 0x0008,
    BUTTON_MENU = 0x0010,
    BUTTON_LEFT_THUMBSTICK = 0x0020,
    BUTTON_RIGHT_THUMBSTICK = 0x0040,
    BUTTON_LEFT_TRIGGER = 0x0080,
    BUTTON_RIGHT_TRIGGER = 0x0100,
    BUTTON_LEFT_GRIP = 0x0200,
    BUTTON_RIGHT_GRIP = 0x0400,
};

enum TrackingFlags : uint32_t
{
    TRACKING_FLAG_LEFT_HAND_ACTIVE = 0x0001,
    TRACKING_FLAG_RIGHT_HAND_ACTIVE = 0x0002,
    TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE = 0x0004,
    TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE = 0x0008,
};

// ─── Control Channel (bidirectional, UDP on CONTROL_PORT) ───────────────────

enum class ControlType : uint8_t
{
    // Keep control messages in a separate value range from MessageType since
    // discovery/control currently share the same UDP socket on CONTROL_PORT.
    BitrateUpdate = 0x81,      // Server → Client: bitrate changed
    LatencyReport = 0x82,      // Client → Server: measured client-side latencies
    RequestKeyframe = 0x83,    // Client → Server: force IDR
    Haptics = 0x84,            // Server → Client: vibration feedback
    NackRequest = 0x85,        // Client → Server: retransmit specific packets
};

struct LatencyReport
{
    ControlType type = ControlType::LatencyReport;
    uint8_t reserved[3] = {};
    float receiveToDecoderSubmitMs;
    float decodeLatencyMs;
    float compositorLatencyMs;
    float totalClientLatencyMs;
};

enum KeyframeReasonFlags : uint32_t
{
    KEYFRAME_REASON_FRAME_LOSS = 0x01,
    KEYFRAME_REASON_DECODE_STALL = 0x02,
    KEYFRAME_REASON_STREAM_RECOVERY = 0x04,
};

struct RequestKeyframe
{
    ControlType type = ControlType::RequestKeyframe;
    uint8_t reserved[3] = {};
    uint32_t reasonFlags = 0;
    uint32_t detail = 0;       // Dropped frame count or stall age in ms
};

struct HapticsCommand
{
    ControlType type = ControlType::Haptics;
    uint8_t hand;              // 0 = left, 1 = right
    uint8_t reserved[2] = {};
    float amplitude;           // 0.0 - 1.0
    float durationMs;
    float frequency;           // Hz, 0 = default
};

// NACK: request retransmission of specific missing packets within a frame.
// Bitmask covers up to 64 packets starting from packetIndexStart.
// For frames with >64 packets, multiple NackRequests can be sent.
struct NackRequest
{
    ControlType type = ControlType::NackRequest;
    uint8_t reserved[3] = {};
    uint32_t frameIndex = 0;
    uint16_t packetIndexStart = 0;  // First packet index this bitmask covers
    uint16_t totalPackets = 0;      // Total packets in the frame (for validation)
    uint64_t missingBitmask = 0;    // Bit i = packet (packetIndexStart + i) is missing
};

} // namespace protocol
} // namespace oxr
