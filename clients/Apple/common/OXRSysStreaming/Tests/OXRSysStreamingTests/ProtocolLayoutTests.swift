// SPDX-License-Identifier: MPL-2.0

import XCTest
@testable import OXRSysStreaming

final class ProtocolLayoutTests: XCTestCase {
    func testDiscoveryLayoutsMatchCppWireFormat() {
        XCTAssertEqual(OXRProtocol.serverAnnounceBaseSize, 92)
        XCTAssertEqual(OXRProtocol.clientConnectBaseSize, 80)
        XCTAssertEqual(OXRProtocol.latencyReportBaseSize, 20)
        XCTAssertEqual(MemoryLayout<ServerAnnounce>.size, 152)
        XCTAssertEqual(MemoryLayout<ServerAnnounce>.offset(of: \.serverFeatures), OXRProtocol.serverAnnounceBaseSize)
        XCTAssertEqual(MemoryLayout<ServerAnnounce>.offset(of: \.spatialPort), 144)
        XCTAssertEqual(MemoryLayout<ClientConnect>.size, 96)
        XCTAssertEqual(MemoryLayout<ClientConnect>.offset(of: \.clientCapabilities), OXRProtocol.clientConnectBaseSize)
        XCTAssertEqual(OXRProtocol.streamingMinBitrateMbps, 1)
        XCTAssertEqual(OXRProtocol.streamingMaxBitrateMbps, 200)
        XCTAssertEqual(OXRProtocol.clientMaxBitrateUseServerConfig, 0)
        XCTAssertEqual(OXRProtocol.spatialPort, 9948)
        XCTAssertEqual(ServerFeatureFlags.streamReconfigure, 0x00000010)
        XCTAssertEqual(ClientCapabilityFlags.streamReconfigure, 0x00000010)
        XCTAssertEqual(ClientConnect().maxBitrateMbps, OXRProtocol.clientMaxBitrateUseServerConfig)
    }

    func testVideoAndControlLayoutsMatchCppWireFormat() {
        XCTAssertEqual(MemoryLayout<VideoPacketHeader>.size, 24)
        XCTAssertEqual(MemoryLayout<VideoPacketHeader>.offset(of: \.fecGroupLastPacketPayloadSize), 12)
        XCTAssertEqual(MemoryLayout<VideoPacketHeader>.offset(of: \.reserved), 14)
        XCTAssertEqual(MemoryLayout<VideoPacketHeader>.offset(of: \.presentationTimeNs), 16)
        XCTAssertEqual(MemoryLayout<TcpRecordHeader>.size, 12)
        XCTAssertEqual(MemoryLayout<TcpVideoNalHeader>.size, 24)
        XCTAssertEqual(MemoryLayout<TcpRenderPose>.size, 48)
        XCTAssertEqual(MemoryLayout<TcpAudioHeader>.size, 24)
        XCTAssertEqual(OXRProtocol.tcpRecordMagic, 0x4f585255)
        XCTAssertEqual(MemoryLayout<AudioPacketHeader>.size, 32)
        XCTAssertEqual(MemoryLayout<LatencyReport>.size, 40)
        XCTAssertEqual(MemoryLayout<RequestKeyframe>.size, 12)
        XCTAssertEqual(MemoryLayout<HapticsCommand>.size, 16)
        XCTAssertEqual(MemoryLayout<NackRequest>.size, 24)
        XCTAssertEqual(MemoryLayout<StreamConfigUpdate>.size, 68)
        XCTAssertEqual(MemoryLayout<StreamConfigAck>.size, 16)
    }

    func testTrackingLayoutMatchesCppWireFormat() {
        XCTAssertEqual(MemoryLayout<TrackingPacket>.size, 1008)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.headLinearVelocity), 152)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.headAngularVelocity), 164)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.leftHandJoints), 176)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.rightHandJoints), 592)
        XCTAssertEqual(TrackingFlagsValues.leftControllerActive, 0x0004)
        XCTAssertEqual(TrackingFlagsValues.rightControllerActive, 0x0008)
    }
}
