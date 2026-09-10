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
        XCTAssertEqual(MemoryLayout<ClientConnect>.offset(of: \.supportedCodecs), 88)
        XCTAssertEqual(OXRProtocol.streamingMinBitrateMbps, 1)
        XCTAssertEqual(OXRProtocol.streamingMaxBitrateMbps, 200)
        XCTAssertEqual(OXRProtocol.clientMaxBitrateUseServerConfig, 0)
        XCTAssertEqual(OXRProtocol.spatialPort, 9948)
        XCTAssertEqual(MessageType.discoveryRequest.rawValue, 0x04)
        XCTAssertEqual(ServerFeatureFlags.streamReconfigure, 0x00000010)
        XCTAssertEqual(ClientCapabilityFlags.streamReconfigure, 0x00000010)
        XCTAssertEqual(ClientCapabilityFlags.tenBitEncoding, 0x00000400)
        XCTAssertEqual(VideoCodec.h264.rawValue, 1)
        XCTAssertEqual(ClientCodecCapability.h265, 0x00000001)
        XCTAssertEqual(ClientCodecCapability.h264, 0x00000002)
        XCTAssertEqual(ClientConnect().preferredCodec, VideoCodec.h265.rawValue)
        XCTAssertEqual(ClientConnect().maxBitrateMbps, OXRProtocol.clientMaxBitrateUseServerConfig)
    }

    func testDirectDiscoveryHostValidation() {
        XCTAssertEqual(DiscoveryClient.normalizedDirectHost(" 192.168.1.20 "), "192.168.1.20")
        XCTAssertEqual(DiscoveryClient.normalizedDirectHost("studio-mac.local"), "studio-mac.local")
        XCTAssertEqual(DiscoveryClient.normalizedDirectHost("runtime.example."), "runtime.example")
        XCTAssertNil(DiscoveryClient.normalizedDirectHost(""))
        XCTAssertNil(DiscoveryClient.normalizedDirectHost("http://runtime.local"))
        XCTAssertNil(DiscoveryClient.normalizedDirectHost("runtime.local:9946"))
        XCTAssertNil(DiscoveryClient.normalizedDirectHost("bad host.local"))
        XCTAssertNil(DiscoveryClient.normalizedDirectHost("-runtime.local"))
    }

    func testDirectDiscoveryRequestsAndParsesCompleteAnnounce() throws {
        let socket = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        XCTAssertGreaterThanOrEqual(socket, 0)
        guard socket >= 0 else { return }
        defer { close(socket) }

        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = OXRProtocol.controlPort.bigEndian
        address.sin_addr.s_addr = INADDR_LOOPBACK.bigEndian
        let bindResult = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
                Darwin.bind(socket, socketAddress, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bindResult == 0 else {
            throw XCTSkip("UDP control port \(OXRProtocol.controlPort) is already in use")
        }

        let serverFinished = expectation(description: "mock server replied")
        DispatchQueue.global(qos: .userInitiated).async {
            var request: UInt8 = 0
            var sender = sockaddr_in()
            var senderLength = socklen_t(MemoryLayout<sockaddr_in>.size)
            let received = withUnsafeMutablePointer(to: &sender) { pointer in
                pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
                    recvfrom(socket, &request, MemoryLayout<UInt8>.size, 0,
                             socketAddress, &senderLength)
                }
            }
            guard received == MemoryLayout<UInt8>.size,
                  request == MessageType.discoveryRequest.rawValue else { return }

            var announce = ServerAnnounce()
            announce.encodedWidth = 3_840
            announce.encodedHeight = 2_160
            announce.refreshRateHz = 90
            announce.serverFeatures = ServerFeatureFlags.foveatedEncoding
            withUnsafeBytes(of: &announce) { bytes in
                withUnsafePointer(to: &sender) { pointer in
                    pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
                        _ = sendto(socket, bytes.baseAddress, bytes.count, 0,
                                   socketAddress, senderLength)
                    }
                }
            }
            serverFinished.fulfill()
        }

        let clientFinished = expectation(description: "client parsed announce")
        let client = DiscoveryClient()
        client.requestServer(at: "127.0.0.1") { server in
            XCTAssertEqual(server.address, "127.0.0.1")
            XCTAssertEqual(server.announce.encodedWidth, 3_840)
            XCTAssertEqual(server.announce.encodedHeight, 2_160)
            XCTAssertEqual(server.announce.refreshRateHz, 90)
            XCTAssertEqual(server.announce.serverFeatures, ServerFeatureFlags.foveatedEncoding)
            clientFinished.fulfill()
        } onError: { message in
            XCTFail(message)
            clientFinished.fulfill()
        }

        wait(for: [serverFinished, clientFinished], timeout: 4)
        client.stop()
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
        XCTAssertEqual(MemoryLayout<TrackingPacket>.size, 1064)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.headLinearVelocity), 152)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.headAngularVelocity), 164)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.leftHandJoints), 176)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.rightHandJoints), 592)
        // Aim pose appended after the hand-joint payload (must match C++ Protocol.h layout).
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.leftControllerAimPos), 1008)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.leftControllerAimRot), 1020)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.rightControllerAimPos), 1036)
        XCTAssertEqual(MemoryLayout<TrackingPacket>.offset(of: \.rightControllerAimRot), 1048)
        XCTAssertEqual(TrackingFlagsValues.leftControllerActive, 0x0004)
        XCTAssertEqual(TrackingFlagsValues.rightControllerActive, 0x0008)
    }
}
