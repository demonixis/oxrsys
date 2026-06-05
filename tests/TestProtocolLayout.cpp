// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include <oxrsys/protocol/Protocol.h>

#include <cstddef>

using namespace oxr::protocol;

TEST_CASE("C++ protocol layouts match the documented wire format", "[protocol]")
{
    STATIC_REQUIRE(sizeof(ServerAnnounce) == 92);
    STATIC_REQUIRE(sizeof(ClientConnect) == 80);
    STATIC_REQUIRE(sizeof(VideoPacketHeader) == 24);
    STATIC_REQUIRE(offsetof(VideoPacketHeader, fecGroupLastPacketPayloadSize) == 12);
    STATIC_REQUIRE(offsetof(VideoPacketHeader, reserved) == 14);
    STATIC_REQUIRE(offsetof(VideoPacketHeader, presentationTimeNs) == 16);
    STATIC_REQUIRE(sizeof(TcpRecordHeader) == 12);
    STATIC_REQUIRE(sizeof(TcpVideoNalHeader) == 24);
    STATIC_REQUIRE(sizeof(TcpRenderPose) == 48);
    STATIC_REQUIRE(TCP_RECORD_MAGIC == 0x4f585255);
    STATIC_REQUIRE(STREAMING_MIN_BITRATE_MBPS == 1);
    STATIC_REQUIRE(STREAMING_MAX_BITRATE_MBPS == 200);
    STATIC_REQUIRE(CLIENT_MAX_BITRATE_USE_SERVER_CONFIG == 0);

    STATIC_REQUIRE(sizeof(LatencyReport) == 20);
    STATIC_REQUIRE(sizeof(RequestKeyframe) == 12);
    STATIC_REQUIRE(sizeof(HapticsCommand) == 16);
    STATIC_REQUIRE(sizeof(NackRequest) == 24);

    STATIC_REQUIRE(sizeof(TrackingPacket) == 1008);
    STATIC_REQUIRE(offsetof(TrackingPacket, headLinearVelocity) == 152);
    STATIC_REQUIRE(offsetof(TrackingPacket, headAngularVelocity) == 164);
    STATIC_REQUIRE(offsetof(TrackingPacket, leftHandJoints) == 176);
    STATIC_REQUIRE(offsetof(TrackingPacket, rightHandJoints) == 592);
    STATIC_REQUIRE(TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE == 0x0004);
    STATIC_REQUIRE(TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE == 0x0008);
}
