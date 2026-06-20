// SPDX-License-Identifier: MPL-2.0

// Protocol.swift — Swift port of common/protocol/include/oxrsys/protocol/Protocol.h
// Binary-compatible with the C++ structs for UDP wire format.

import Foundation

public enum OXRProtocol {
    public static let discoveryPort: UInt16 = 9943
    public static let videoPort: UInt16 = 9944
    public static let trackingPort: UInt16 = 9945
    public static let controlPort: UInt16 = 9946
    public static let audioPort: UInt16 = 9947
    public static let spatialPort: UInt16 = 9948
    public static let handJointCount: Int = 26
    public static let streamingMinBitrateMbps: UInt32 = 1
    public static let streamingMaxBitrateMbps: UInt32 = 200
    public static let clientMaxBitrateUseServerConfig: UInt32 = 0
    public static let serverAnnounceBaseSize: Int = 92
    public static let clientConnectBaseSize: Int = 80
    public static let latencyReportBaseSize: Int = 20
    public static let maxPacketPayload: Int = 1400
    public static let tcpRecordMagic: UInt32 = 0x4f585255
    public static let tcpRecordVersion: UInt16 = 1
    public static let tcpMaxRecordPayload: UInt32 = 16 * 1024 * 1024
}

public enum StreamingTransport: UInt8, Sendable {
    case auto = 0
    case wifi = 1
    case usbAdb = 2
}

public enum TcpRecordType: UInt16, Sendable {
    case serverAnnounce = 0x0001
    case clientConnect = 0x0002
    case videoNal = 0x0003
    case renderPose = 0x0004
    case tracking = 0x0005
    case control = 0x0006
    case disconnect = 0x0007
    case audio = 0x0008
    case spatial = 0x0009
}

public struct TcpRecordHeader: Sendable {
    public var magic: UInt32 = OXRProtocol.tcpRecordMagic
    public var version: UInt16 = OXRProtocol.tcpRecordVersion
    public var type: UInt16 = TcpRecordType.control.rawValue
    public var payloadSize: UInt32 = 0

    public init() {}
}

public struct TcpVideoNalHeader: Sendable {
    public var presentationTimeNs: Int64 = 0
    public var frameIndex: UInt32 = 0
    public var payloadSize: UInt32 = 0
    public var flags: UInt8 = 0
    public var codec: UInt8 = 0
    public var reserved: UInt16 = 0
    public var reserved2: UInt32 = 0

    public init() {}
}

public struct TcpRenderPose: Sendable {
    public var presentationTimeNs: Int64 = 0
    public var frameIndex: UInt32 = 0
    public var reserved: UInt32 = 0
    public var position: (Float, Float, Float) = (0, 0, 0)
    public var orientation: (Float, Float, Float, Float) = (0, 0, 0, 1)
    public var reserved2: UInt32 = 0

    public init() {}
}

public struct TcpAudioHeader: Sendable {
    public var presentationTimeNs: Int64 = 0
    public var frameCount: UInt32 = 0
    public var payloadSize: UInt32 = 0
    public var sampleRateHz: UInt32 = 48000
    public var channels: UInt16 = 2
    public var format: UInt16 = AudioSampleFormat.float32.rawValue

    public init() {}
}

// MARK: - Discovery

public enum MessageType: UInt8, Sendable {
    case serverAnnounce = 0x01
    case clientConnect = 0x02
    case serverDisconnect = 0x03
}

public struct ServerFeatureFlags {
    public static let foveatedEncoding: UInt32 = 0x00000001
    // clientFoveationPreset is an explicit headset-client override.
    public static let clientFoveation: UInt32 = 0x00000002
    public static let clientUpscaling: UInt32 = 0x00000004
    public static let headsetAudio: UInt32 = 0x00000008
    public static let streamReconfigure: UInt32 = 0x00000010
    public static let mixedRealityPassthrough: UInt32 = 0x00000020
    public static let mixedRealityAlpha: UInt32 = 0x00000040
    public static let depthOcclusion: UInt32 = 0x00000080
    public static let spatialEntity: UInt32 = 0x00000100
    public static let sceneCapture: UInt32 = 0x00000200
}

public struct ClientCapabilityFlags {
    public static let foveatedEncoding: UInt32 = 0x00000001
    public static let clientFoveation: UInt32 = 0x00000002
    public static let clientUpscaling: UInt32 = 0x00000004
    public static let audioOutput: UInt32 = 0x00000008
    public static let streamReconfigure: UInt32 = 0x00000010
    public static let mixedRealityPassthrough: UInt32 = 0x00000020
    public static let mixedRealityAlpha: UInt32 = 0x00000040
    public static let depthOcclusion: UInt32 = 0x00000080
    public static let spatialEntity: UInt32 = 0x00000100
    public static let sceneCapture: UInt32 = 0x00000200
}

public enum FoveationPreset: UInt32, Sendable {
    case off = 0
    case light = 1
    case medium = 2
    case high = 3
}

public enum ClientFoveationPreset: UInt32, Sendable {
    case off = 0
    case light = 1
    case medium = 2
    case high = 3
}

public enum ClientUpscalingMode: UInt32, Sendable {
    case off = 0
    case snapdragonGsr = 1
}

public enum ClientReprojectionMode: UInt32, Sendable {
    case off = 0
    case pose = 1
    case poseWarp = 2
}

public enum AudioSampleFormat: UInt16, Sendable {
    case float32 = 1
}

public struct ServerAnnounce: Sendable {
    public var type: UInt8 = MessageType.serverAnnounce.rawValue
    public var versionMajor: UInt8 = 1
    public var versionMinor: UInt8 = 0
    public var reserved: UInt8 = 0
    public var videoPort: UInt16 = OXRProtocol.videoPort
    public var trackingPort: UInt16 = OXRProtocol.trackingPort
    public var renderWidth: UInt32 = 0
    public var renderHeight: UInt32 = 0
    public var refreshRateHz: UInt32 = 0
    public var encodedWidth: UInt32 = 0
    public var encodedHeight: UInt32 = 0
    public var serverName: (
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8
    ) = (
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    )

    public var serverFeatures: UInt32 = 0
    public var audioPort: UInt32 = UInt32(OXRProtocol.audioPort)
    public var foveatedEncodingPreset: UInt32 = FoveationPreset.off.rawValue
    public var clientFoveationPreset: UInt32 = ClientFoveationPreset.off.rawValue
    public var clientUpscalingMode: UInt32 = ClientUpscalingMode.off.rawValue
    public var clientReprojectionMode: UInt32 = ClientReprojectionMode.pose.rawValue
    public var audioSampleRateHz: UInt32 = 48000
    public var foveationCenterSizeX: Float = 0
    public var foveationCenterSizeY: Float = 0
    public var foveationCenterShiftX: Float = 0
    public var foveationCenterShiftY: Float = 0
    public var foveationEdgeRatioX: Float = 1
    public var foveationEdgeRatioY: Float = 1
    public var spatialPort: UInt32 = UInt32(OXRProtocol.spatialPort)
    public var reserved2: UInt32 = 0

    public init() {}

    public var serverNameString: String {
        withUnsafeBytes(of: serverName) { buf in
            let bytes = buf.bindMemory(to: UInt8.self)
            let len = bytes.firstIndex(of: 0) ?? 64
            return String(bytes: bytes[..<len], encoding: .utf8) ?? "Unknown"
        }
    }
}

public struct ClientConnect: Sendable {
    public var type: UInt8 = MessageType.clientConnect.rawValue
    public var versionMajor: UInt8 = 1
    public var versionMinor: UInt8 = 0
    public var reserved: UInt8 = 0
    public var preferredCodec: UInt32 = 0 // H265
    public var maxBitrateMbps: UInt32 = OXRProtocol.clientMaxBitrateUseServerConfig
    public var refreshRateHz: UInt32 = 0
    public var deviceName: (
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
        UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8
    ) = (
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    )
    public var clientCapabilities: UInt32 = 0
    public var audioSampleRateHz: UInt32 = 48000
    public var reserved2: UInt32 = 0
    public var reserved3: UInt32 = 0

    public init() {}

    public mutating func setDeviceName(_ name: String) {
        withUnsafeMutableBytes(of: &deviceName) { buf in
            buf.storeBytes(of: 0, as: UInt8.self) // zero-fill
            let bytes = Array(name.utf8.prefix(63))
            for (i, b) in bytes.enumerated() {
                buf[i] = b
            }
            buf[bytes.count] = 0
        }
    }
}

// MARK: - Video

public enum VideoCodec: UInt32, Sendable {
    case h265 = 0
    case h264 = 1
    case av1 = 2
}

public struct VideoPacketHeader: Sendable {
    public var frameIndex: UInt32 = 0
    public var packetIndex: UInt16 = 0
    public var totalPackets: UInt16 = 0
    public var payloadSize: UInt16 = 0
    public var flags: UInt8 = 0
    public var codec: UInt8 = 0
    public var fecGroupLastPacketPayloadSize: UInt16 = 0
    public var reserved: UInt16 = 0
    public var presentationTimeNs: Int64 = 0

    public init() {}
}

public struct VideoFlags {
    public static let keyframe: UInt8 = 0x01
    public static let endOfFrame: UInt8 = 0x02
    public static let leftEye: UInt8 = 0x04
    public static let rightEye: UInt8 = 0x08
    public static let stereo: UInt8 = 0x0C
    public static let fec: UInt8 = 0x10
    public static let renderPose: UInt8 = 0x20
    public static let alphaBlend: UInt8 = 0x40
}

public struct AudioPacketHeader: Sendable {
    public var presentationTimeNs: Int64 = 0
    public var frameIndex: UInt32 = 0
    public var frameCount: UInt32 = 0
    public var payloadSize: UInt32 = 0
    public var sampleRateHz: UInt32 = 48000
    public var channels: UInt16 = 2
    public var format: UInt16 = AudioSampleFormat.float32.rawValue
    public var reserved: UInt32 = 0

    public init() {}
}

public enum FEC {
    /// Number of data packets per FEC group. Must match server (Protocol.h FEC_GROUP_SIZE).
    public static let groupSize: Int = 10
}

// MARK: - Tracking

public struct TrackingPacket: Sendable {
    public var timestampNs: Int64 = 0
    public var trackingFlags: UInt32 = 0

    // Head pose
    public var headPosition: (Float, Float, Float) = (0, 0, 0)
    public var headOrientation: (Float, Float, Float, Float) = (0, 0, 0, 1) // (x, y, z, w)

    // Left controller
    public var leftControllerPos: (Float, Float, Float) = (0, 0, 0)
    public var leftControllerRot: (Float, Float, Float, Float) = (0, 0, 0, 1)

    // Right controller
    public var rightControllerPos: (Float, Float, Float) = (0, 0, 0)
    public var rightControllerRot: (Float, Float, Float, Float) = (0, 0, 0, 1)

    // Input state
    public var buttonState: UInt32 = 0
    public var leftTrigger: Float = 0
    public var rightTrigger: Float = 0
    public var leftGrip: Float = 0
    public var rightGrip: Float = 0

    // Thumbsticks
    public var leftThumbstick: (Float, Float) = (0, 0)
    public var rightThumbstick: (Float, Float) = (0, 0)

    // Eye data
    public var ipd: Float = 0
    public var eyeFov: (Float, Float, Float, Float) = (0, 0, 0, 0)

    // Velocity from the client IMU, used by the runtime for bounded pose prediction.
    public var headLinearVelocity: (Float, Float, Float) = (0, 0, 0)
    public var headAngularVelocity: (Float, Float, Float) = (0, 0, 0)

    // Hand joints: 26 joints x 4 floats (x, y, z, radius) per hand = 832 bytes
    public var leftHandJoints: HandJointData = HandJointData()
    public var rightHandJoints: HandJointData = HandJointData()

    public init() {}
}

public struct HandJointData: Sendable {
    // 26 joints x 4 floats = 104 floats = 416 bytes
    public var data: (
        Float, Float, Float, Float, Float, Float, Float, Float,
        Float, Float, Float, Float, Float, Float, Float, Float,
        Float, Float, Float, Float, Float, Float, Float, Float,
        Float, Float, Float, Float, Float, Float, Float, Float,
        Float, Float, Float, Float, Float, Float, Float, Float,
        Float, Float, Float, Float, Float, Float, Float, Float,
        Float, Float, Float, Float, Float, Float, Float, Float,
        Float, Float, Float, Float, Float, Float, Float, Float,
        Float, Float, Float, Float, Float, Float, Float, Float,
        Float, Float, Float, Float, Float, Float, Float, Float,
        Float, Float, Float, Float, Float, Float, Float, Float,
        Float, Float, Float, Float, Float, Float, Float, Float,
        Float, Float, Float, Float, Float, Float, Float, Float
    ) = (
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0
    )

    public init() {}

    public mutating func setJoint(index: Int, x: Float, y: Float, z: Float, radius: Float) {
        withUnsafeMutableBytes(of: &data) { buf in
            let floats = buf.bindMemory(to: Float.self)
            let base = index * 4
            guard base + 3 < floats.count else { return }
            floats[base] = x
            floats[base + 1] = y
            floats[base + 2] = z
            floats[base + 3] = radius
        }
    }
}

public struct TrackingFlagsValues {
    public static let leftHandActive: UInt32 = 0x0001
    public static let rightHandActive: UInt32 = 0x0002
    public static let leftControllerActive: UInt32 = 0x0004
    public static let rightControllerActive: UInt32 = 0x0008
}

public struct ButtonFlags {
    public static let a: UInt32 = 0x0001
    public static let b: UInt32 = 0x0002
    public static let x: UInt32 = 0x0004
    public static let y: UInt32 = 0x0008
    public static let menu: UInt32 = 0x0010
}

// MARK: - Control Channel

public enum ControlType: UInt8, Sendable {
    case bitrateUpdate = 0x81
    case latencyReport = 0x82
    case requestKeyframe = 0x83
    case haptics = 0x84
    case nackRequest = 0x85
    case streamConfigUpdate = 0x86
    case streamConfigAck = 0x87
}

public struct StreamConfigUpdateFlags {
    public static let reconfigureDecoder: UInt32 = 0x00000001
    public static let forceKeyframe: UInt32 = 0x00000002
    public static let foveatedEncoding: UInt32 = 0x00000004
    public static let clientUpscaling: UInt32 = 0x00000008
}

public struct StreamConfigAckStatus {
    public static let ok: UInt8 = 0
    public static let rejected: UInt8 = 1
}

public struct NackRequest: Sendable {
    public var type: UInt8 = ControlType.nackRequest.rawValue
    public var reserved: (UInt8, UInt8, UInt8) = (0, 0, 0)
    public var frameIndex: UInt32 = 0
    public var packetIndexStart: UInt16 = 0
    public var totalPackets: UInt16 = 0
    public var missingBitmask: UInt64 = 0

    public init() {}
}

public struct LatencyReport: Sendable {
    public var type: UInt8 = ControlType.latencyReport.rawValue
    public var reserved: (UInt8, UInt8, UInt8) = (0, 0, 0)
    public var receiveToDecoderSubmitMs: Float = 0
    public var decodeLatencyMs: Float = 0
    public var compositorLatencyMs: Float = 0
    public var totalClientLatencyMs: Float = 0
    public var displayedFrameAgeMs: Float = 0
    public var reprojectedFrames: UInt32 = 0
    public var staleFrameReuses: UInt32 = 0
    public var renderPoseFallbacks: UInt32 = 0
    public var reprojectionMode: UInt32 = ClientReprojectionMode.off.rawValue

    public init() {}
}

public struct RequestKeyframe: Sendable {
    public var type: UInt8 = ControlType.requestKeyframe.rawValue
    public var reserved: (UInt8, UInt8, UInt8) = (0, 0, 0)
    public var reasonFlags: UInt32 = 0
    public var detail: UInt32 = 0

    public init() {}
}

public struct HapticsCommand: Sendable {
    public var type: UInt8 = ControlType.haptics.rawValue
    public var hand: UInt8 = 0
    public var reserved: (UInt8, UInt8) = (0, 0)
    public var amplitude: Float = 0
    public var durationMs: Float = 0
    public var frequency: Float = 0

    public init() {}
}

public struct StreamConfigUpdate: Sendable {
    public var type: UInt8 = ControlType.streamConfigUpdate.rawValue
    public var reserved: (UInt8, UInt8, UInt8) = (0, 0, 0)
    public var sequence: UInt32 = 0
    public var renderWidth: UInt32 = 0
    public var renderHeight: UInt32 = 0
    public var encodedWidth: UInt32 = 0
    public var encodedHeight: UInt32 = 0
    public var targetBitrateMbps: UInt32 = 0
    public var refreshRateHz: UInt32 = 0
    public var flags: UInt32 = 0
    public var foveatedEncodingPreset: UInt32 = FoveationPreset.off.rawValue
    public var clientUpscalingMode: UInt32 = ClientUpscalingMode.off.rawValue
    public var foveationCenterSizeX: Float = 0
    public var foveationCenterSizeY: Float = 0
    public var foveationCenterShiftX: Float = 0
    public var foveationCenterShiftY: Float = 0
    public var foveationEdgeRatioX: Float = 1
    public var foveationEdgeRatioY: Float = 1

    public init() {}
}

public struct StreamConfigAck: Sendable {
    public var type: UInt8 = ControlType.streamConfigAck.rawValue
    public var status: UInt8 = StreamConfigAckStatus.ok
    public var reserved: (UInt8, UInt8) = (0, 0)
    public var sequence: UInt32 = 0
    public var encodedWidth: UInt32 = 0
    public var encodedHeight: UInt32 = 0

    public init() {}
}

public enum KeyframeReason: UInt32, Sendable {
    case frameLoss = 0x01
    case decodeStall = 0x02
    case streamRecovery = 0x04
}
