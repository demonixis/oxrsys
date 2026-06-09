// SPDX-License-Identifier: MPL-2.0

import XCTest
@testable import OXRSysStreaming

final class ProtocolLayoutTests: XCTestCase {
    func testDiscoveryLayoutsMatchCppWireFormat() {
        XCTAssertEqual(MemoryLayout<ServerAnnounce>.size, 92)
        XCTAssertEqual(MemoryLayout<ClientConnect>.size, 80)
        XCTAssertEqual(OXRProtocol.streamingMinBitrateMbps, 1)
        XCTAssertEqual(OXRProtocol.streamingMaxBitrateMbps, 200)
        XCTAssertEqual(OXRProtocol.clientMaxBitrateUseServerConfig, 0)
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
        XCTAssertEqual(OXRProtocol.tcpRecordMagic, 0x4f585255)
        XCTAssertEqual(MemoryLayout<LatencyReport>.size, 20)
        XCTAssertEqual(MemoryLayout<RequestKeyframe>.size, 12)
        XCTAssertEqual(MemoryLayout<HapticsCommand>.size, 16)
        XCTAssertEqual(MemoryLayout<NackRequest>.size, 24)
    }

    func testTrackingLayoutMatchesCppWireFormat() {
        XCTAssertEqual(MemoryLayout<TrackingPacket>.size, 1008)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.headLinearVelocity), 152)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.headAngularVelocity), 164)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.leftHandJoints), 176)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.rightHandJoints), 592)
        XCTAssertEqual(TrackingFlagsValues.leftControllerActive, 0x0004)
        XCTAssertEqual(TrackingFlagsValues.rightControllerActive, 0x0008)
        XCTAssertEqual(ButtonFlags.leftTrigger, 0x0080)
        XCTAssertEqual(ButtonFlags.rightTrigger, 0x0100)
        XCTAssertEqual(ButtonFlags.leftGrip, 0x0200)
        XCTAssertEqual(ButtonFlags.rightGrip, 0x0400)
    }
}
