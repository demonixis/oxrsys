// SPDX-License-Identifier: MPL-2.0

import XCTest
@testable import OXRSysStreaming

final class ProtocolLayoutTests: XCTestCase {
    func testDiscoveryLayoutsMatchCppWireFormat() {
        XCTAssertEqual(MemoryLayout<ServerAnnounce>.size, 92)
        XCTAssertEqual(MemoryLayout<ClientConnect>.size, 80)
    }

    func testVideoAndControlLayoutsMatchCppWireFormat() {
        XCTAssertEqual(MemoryLayout<VideoPacketHeader>.size, 24)
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
    }
}
