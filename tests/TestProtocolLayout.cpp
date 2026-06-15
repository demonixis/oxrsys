// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include <oxrsys/protocol/Foveation.h>
#include <oxrsys/protocol/Protocol.h>

#include <cstddef>

using namespace oxr::protocol;

TEST_CASE("C++ protocol layouts match the documented wire format", "[protocol]")
{
    STATIC_REQUIRE(SERVER_ANNOUNCE_BASE_SIZE == 92);
    STATIC_REQUIRE(CLIENT_CONNECT_BASE_SIZE == 80);
    STATIC_REQUIRE(sizeof(ServerAnnounce) == 140);
    STATIC_REQUIRE(offsetof(ServerAnnounce, serverFeatures) == SERVER_ANNOUNCE_BASE_SIZE);
    STATIC_REQUIRE(sizeof(ClientConnect) == 96);
    STATIC_REQUIRE(offsetof(ClientConnect, clientCapabilities) == CLIENT_CONNECT_BASE_SIZE);
    STATIC_REQUIRE(sizeof(VideoPacketHeader) == 24);
    STATIC_REQUIRE(offsetof(VideoPacketHeader, fecGroupLastPacketPayloadSize) == 12);
    STATIC_REQUIRE(offsetof(VideoPacketHeader, reserved) == 14);
    STATIC_REQUIRE(offsetof(VideoPacketHeader, presentationTimeNs) == 16);
    STATIC_REQUIRE(sizeof(TcpRecordHeader) == 12);
    STATIC_REQUIRE(sizeof(TcpVideoNalHeader) == 24);
    STATIC_REQUIRE(sizeof(TcpRenderPose) == 48);
    STATIC_REQUIRE(sizeof(TcpAudioHeader) == 24);
    STATIC_REQUIRE(sizeof(AudioPacketHeader) == 32);
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

TEST_CASE("Foveated encoding presets calculate ALVR-style optimized eye sizes", "[protocol][foveation]")
{
    const FoveationLayout light =
        CalculateFoveationLayout(2144, 2144, FoveationPreset::Light);
    CHECK(light.optimizedEyeWidth == 1728);
    CHECK(light.optimizedEyeHeight == 1504);
    CHECK(light.parameters.edgeRatioX == 2.0f);
    CHECK(light.parameters.edgeRatioY == 3.0f);

    const FoveationLayout medium =
        CalculateFoveationLayout(2144, 2144, FoveationPreset::Medium);
    CHECK(medium.optimizedEyeWidth == 1280);
    CHECK(medium.optimizedEyeHeight == 1120);
    CHECK(medium.eyeWidthRatio > 0.98f);
    CHECK(medium.eyeHeightRatio > 0.97f);

    const FoveationLayout high =
        CalculateFoveationLayout(2144, 2144, FoveationPreset::High);
    CHECK(high.optimizedEyeWidth == 992);
    CHECK(high.optimizedEyeHeight == 896);

    const FoveationLayout off =
        CalculateFoveationLayout(2144, 2144, FoveationPreset::Off);
    CHECK(off.optimizedEyeWidth == 2144);
    CHECK(off.optimizedEyeHeight == 2144);
}
