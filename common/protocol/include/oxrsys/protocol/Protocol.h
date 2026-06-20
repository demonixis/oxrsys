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
constexpr uint16_t AUDIO_PORT = 9947;
constexpr uint16_t SPATIAL_PORT = 9948;
constexpr uint32_t HAND_JOINT_COUNT = 26;
constexpr uint32_t STREAMING_MIN_BITRATE_MBPS = 1;
constexpr uint32_t STREAMING_MAX_BITRATE_MBPS = 200;
constexpr uint32_t CLIENT_MAX_BITRATE_USE_SERVER_CONFIG = 0;
constexpr uint32_t SERVER_ANNOUNCE_BASE_SIZE = 92;
constexpr uint32_t CLIENT_CONNECT_BASE_SIZE = 80;
constexpr uint32_t LATENCY_REPORT_BASE_SIZE = 20;

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
    Audio = 0x0008,
    Spatial = 0x0009,
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

struct TcpAudioHeader
{
    int64_t presentationTimeNs = 0;
    uint32_t frameCount = 0;
    uint32_t payloadSize = 0;
    uint32_t sampleRateHz = 48000;
    uint16_t channels = 2;
    uint16_t format = 1; // AudioSampleFormat::Float32
};

// ─── Discovery (UDP broadcast on DISCOVERY_PORT) ────────────────────────────

enum class MessageType : uint8_t
{
    ServerAnnounce = 0x01,
    ClientConnect = 0x02,
    ServerDisconnect = 0x03,
};

enum ServerFeatureFlags : uint32_t
{
    SERVER_FEATURE_FOVEATED_ENCODING = 0x00000001,
    SERVER_FEATURE_CLIENT_FOVEATION = 0x00000002, // clientFoveationPreset is an explicit override
    SERVER_FEATURE_CLIENT_UPSCALING = 0x00000004,
    SERVER_FEATURE_HEADSET_AUDIO = 0x00000008,
    SERVER_FEATURE_STREAM_RECONFIGURE = 0x00000010,
    SERVER_FEATURE_MIXED_REALITY_PASSTHROUGH = 0x00000020,
    SERVER_FEATURE_MIXED_REALITY_ALPHA = 0x00000040,
    SERVER_FEATURE_DEPTH_OCCLUSION = 0x00000080,
    SERVER_FEATURE_SPATIAL_ENTITY = 0x00000100,
    SERVER_FEATURE_SCENE_CAPTURE = 0x00000200,
};

enum ClientCapabilityFlags : uint32_t
{
    CLIENT_CAPABILITY_FOVEATED_ENCODING = 0x00000001,
    CLIENT_CAPABILITY_CLIENT_FOVEATION = 0x00000002,
    CLIENT_CAPABILITY_CLIENT_UPSCALING = 0x00000004,
    CLIENT_CAPABILITY_AUDIO_OUTPUT = 0x00000008,
    CLIENT_CAPABILITY_STREAM_RECONFIGURE = 0x00000010,
    CLIENT_CAPABILITY_MIXED_REALITY_PASSTHROUGH = 0x00000020,
    CLIENT_CAPABILITY_MIXED_REALITY_ALPHA = 0x00000040,
    CLIENT_CAPABILITY_DEPTH_OCCLUSION = 0x00000080,
    CLIENT_CAPABILITY_SPATIAL_ENTITY = 0x00000100,
    CLIENT_CAPABILITY_SCENE_CAPTURE = 0x00000200,
};

enum class FoveationPreset : uint32_t
{
    Off = 0,
    Light = 1,
    Medium = 2,
    High = 3,
};

enum class ClientFoveationPreset : uint32_t
{
    Off = 0,
    Light = 1,
    Medium = 2,
    High = 3,
};

enum class ClientUpscalingMode : uint32_t
{
    Off = 0,
    SnapdragonGsr = 1,
};

enum class ClientReprojectionMode : uint32_t
{
    Off = 0,
    Pose = 1,
    PoseWarp = 2,
};

enum class AudioSampleFormat : uint16_t
{
    Float32 = 1,
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

    // Protocol v1.1 trailing fields. The first 92 bytes are the stable v1.0
    // prefix so older clients can still receive and parse a truncated announce.
    uint32_t serverFeatures = 0;        // ServerFeatureFlags
    uint32_t audioPort = AUDIO_PORT;
    FoveationPreset foveatedEncodingPreset = FoveationPreset::Off;
    ClientFoveationPreset clientFoveationPreset = ClientFoveationPreset::Off;
    ClientUpscalingMode clientUpscalingMode = ClientUpscalingMode::Off;
    ClientReprojectionMode clientReprojectionMode = ClientReprojectionMode::Pose;
    uint32_t audioSampleRateHz = 48000;
    float foveationCenterSizeX = 0.0f;
    float foveationCenterSizeY = 0.0f;
    float foveationCenterShiftX = 0.0f;
    float foveationCenterShiftY = 0.0f;
    float foveationEdgeRatioX = 1.0f;
    float foveationEdgeRatioY = 1.0f;

    // Protocol v1.2 trailing fields.
    uint32_t spatialPort = SPATIAL_PORT;
    uint32_t reserved2 = 0;
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

    // Protocol v1.1 trailing fields. The first 80 bytes are the stable v1.0
    // prefix so older servers can still parse the original connect message.
    uint32_t clientCapabilities = 0; // ClientCapabilityFlags
    uint32_t audioSampleRateHz = 48000;
    uint32_t reserved2 = 0;
    uint32_t reserved3 = 0;
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
    uint16_t fecGroupLastPacketPayloadSize; // FEC packets: payload size of this group's last data packet
    uint16_t reserved = 0;
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
    VIDEO_FLAG_ALPHA_BLEND = 0x40, // App submitted alpha-blend environment or source-alpha projection layer
};

struct AudioPacketHeader
{
    int64_t presentationTimeNs = 0;
    uint32_t frameIndex = 0;
    uint32_t frameCount = 0;
    uint32_t payloadSize = 0;
    uint32_t sampleRateHz = 48000;
    uint16_t channels = 2;
    uint16_t format = static_cast<uint16_t>(AudioSampleFormat::Float32);
    uint32_t reserved = 0;
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
    StreamConfigUpdate = 0x86, // Server → Client: live encoded stream dimensions changed
    StreamConfigAck = 0x87,    // Client → Server: decoder accepted/rejected the update
};

enum StreamConfigUpdateFlags : uint32_t
{
    STREAM_CONFIG_FLAG_RECONFIGURE_DECODER = 0x00000001,
    STREAM_CONFIG_FLAG_FORCE_KEYFRAME = 0x00000002,
    STREAM_CONFIG_FLAG_FOVEATED_ENCODING = 0x00000004,
    STREAM_CONFIG_FLAG_CLIENT_UPSCALING = 0x00000008,
};

enum StreamConfigAckStatus : uint8_t
{
    STREAM_CONFIG_ACK_OK = 0,
    STREAM_CONFIG_ACK_REJECTED = 1,
};

struct LatencyReport
{
    ControlType type = ControlType::LatencyReport;
    uint8_t reserved[3] = {};
    float receiveToDecoderSubmitMs;
    float decodeLatencyMs;
    float compositorLatencyMs;
    float totalClientLatencyMs;
    float displayedFrameAgeMs = 0.0f;
    uint32_t reprojectedFrames = 0;
    uint32_t staleFrameReuses = 0;
    uint32_t renderPoseFallbacks = 0;
    ClientReprojectionMode reprojectionMode = ClientReprojectionMode::Off;
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

struct StreamConfigUpdate
{
    ControlType type = ControlType::StreamConfigUpdate;
    uint8_t reserved[3] = {};
    uint32_t sequence = 0;
    uint32_t renderWidth = 0;       // Stereo side-by-side render width
    uint32_t renderHeight = 0;
    uint32_t encodedWidth = 0;
    uint32_t encodedHeight = 0;
    uint32_t targetBitrateMbps = 0;
    uint32_t refreshRateHz = 0;
    uint32_t flags = 0;             // StreamConfigUpdateFlags
    FoveationPreset foveatedEncodingPreset = FoveationPreset::Off;
    ClientUpscalingMode clientUpscalingMode = ClientUpscalingMode::Off;
    float foveationCenterSizeX = 0.0f;
    float foveationCenterSizeY = 0.0f;
    float foveationCenterShiftX = 0.0f;
    float foveationCenterShiftY = 0.0f;
    float foveationEdgeRatioX = 1.0f;
    float foveationEdgeRatioY = 1.0f;
};

struct StreamConfigAck
{
    ControlType type = ControlType::StreamConfigAck;
    uint8_t status = STREAM_CONFIG_ACK_OK;
    uint8_t reserved[2] = {};
    uint32_t sequence = 0;
    uint32_t encodedWidth = 0;
    uint32_t encodedHeight = 0;
};

} // namespace protocol
} // namespace oxr
