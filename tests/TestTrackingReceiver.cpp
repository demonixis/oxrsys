// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TrackingReceiver.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

using namespace oxr::protocol;
using Catch::Matchers::WithinAbs;

TEST_CASE("Tracking receiver accepts legacy prefixes without validating absent fields", "[tracking]")
{
    TrackingReceiver receiver;
    TrackingPacket packet = {};
    packet.headOrientation[3] = 1.0f;
    packet.trackingFlags = TRACKING_FLAG_HEAD_LINEAR_VELOCITY_VALID |
                           TRACKING_FLAG_STAGE_BOUNDS_VALID |
                           TRACKING_FLAG_RIGHT_CONTROLLER_AIM_ANGULAR_VELOCITY_VALID;
    packet.headLinearVelocity[0] = 1.25f;
    packet.stageBoundsWidth = 3.0f;
    packet.stageBoundsHeight = 4.0f;
    packet.rightControllerAimAngularVelocity[2] = 2.0f;

    for (size_t size : {TRACKING_PACKET_BASE_SIZE, size_t{1064}, size_t{1071}})
    {
        CAPTURE(size);
        receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), size);
        TrackingPacket received = {};
        REQUIRE(receiver.GetFreshPose(received));
        CHECK(received.headLinearVelocity[0] == 1.25f);
        CHECK(received.trackingFlags == TRACKING_FLAG_HEAD_LINEAR_VELOCITY_VALID);
    }

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), 1072);
    TrackingPacket received = {};
    REQUIRE(receiver.GetFreshPose(received));
    CHECK((received.trackingFlags & TRACKING_FLAG_STAGE_BOUNDS_VALID) != 0);
    CHECK(received.stageBoundsWidth == 3.0f);
    CHECK(received.stageBoundsHeight == 4.0f);
    CHECK((received.trackingFlags & TRACKING_FLAG_RIGHT_CONTROLLER_AIM_ANGULAR_VELOCITY_VALID) == 0);

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet) - 1);
    REQUIRE(receiver.GetFreshPose(received));
    CHECK((received.trackingFlags & TRACKING_FLAG_RIGHT_CONTROLLER_AIM_ANGULAR_VELOCITY_VALID) == 0);

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    REQUIRE(receiver.GetFreshPose(received));
    CHECK(received.trackingFlags == packet.trackingFlags);
    CHECK(received.rightControllerAimAngularVelocity[2] == 2.0f);
}

TEST_CASE("Tracking receiver rejects missing stale and stopped measurements", "[tracking]")
{
    TrackingReceiver receiver;
    TrackingPacket packet = {};
    packet.headOrientation[3] = 1.0f;
    TrackingPacket received = {};
    CHECK_FALSE(receiver.GetFreshPose(received));
    receiver.InjectPacket(nullptr, sizeof(packet));
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), TRACKING_PACKET_BASE_SIZE - 1);
    CHECK_FALSE(receiver.GetFreshPose(received));

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    REQUIRE(receiver.GetFreshPose(received));
    std::this_thread::sleep_for(std::chrono::milliseconds(275));
    CHECK_FALSE(receiver.GetFreshPose(received));

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    REQUIRE(receiver.GetFreshPose(received));
    receiver.Stop();
    CHECK_FALSE(receiver.GetFreshPose(received));
    CHECK_FALSE(receiver.GetLatestPose(received));
    CHECK_FALSE(receiver.GetPredictedPose(received));
    CHECK_FALSE(receiver.IsReceiving());
}

TEST_CASE("Tracking prediction respects explicitly valid stationary measurements", "[tracking]")
{
    TrackingReceiver receiver;
    receiver.SetPredictionHorizonMs(20.0f);
    TrackingPacket first = {};
    first.timestampNs = 1'000'000'000;
    first.headOrientation[3] = 1.0f;
    TrackingPacket second = first;
    second.timestampNs = 1'010'000'000;
    second.headPosition[0] = 0.02f;
    second.headOrientation[1] = std::sin(0.1f);
    second.headOrientation[3] = std::cos(0.1f);
    second.trackingFlags = TRACKING_FLAG_HEAD_LINEAR_VELOCITY_VALID |
                           TRACKING_FLAG_HEAD_ANGULAR_VELOCITY_VALID;

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&first), sizeof(first));
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&second), sizeof(second));
    TrackingPacket predicted = {};
    REQUIRE(receiver.GetPredictedPose(predicted));
    for (int index = 0; index < 3; ++index)
    {
        CHECK_THAT(predicted.headPosition[index], WithinAbs(second.headPosition[index], 1e-6f));
    }
    for (int index = 0; index < 4; ++index)
    {
        CHECK_THAT(predicted.headOrientation[index], WithinAbs(second.headOrientation[index], 1e-6f));
    }
}

TEST_CASE("Tracking prediction rejects nonfinite reported velocity and retains legacy fallback", "[tracking]")
{
    for (float value : {std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max()})
    {
        for (bool flagged : {false, true})
        {
            CAPTURE(value, flagged);
            TrackingReceiver receiver;
            receiver.SetPredictionHorizonMs(20.0f);
            TrackingPacket first = {};
            first.timestampNs = 1'000'000'000;
            first.headOrientation[3] = 1.0f;
            TrackingPacket second = first;
            second.timestampNs = 1'010'000'000;
            second.headPosition[0] = 0.02f;
            second.headOrientation[1] = std::sin(0.1f);
            second.headOrientation[3] = std::cos(0.1f);
            second.headLinearVelocity[0] = value;
            second.headAngularVelocity[1] = value;
            if (flagged)
            {
                second.trackingFlags = TRACKING_FLAG_HEAD_LINEAR_VELOCITY_VALID |
                                       TRACKING_FLAG_HEAD_ANGULAR_VELOCITY_VALID;
            }

            receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&first), sizeof(first));
            receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&second), sizeof(second));
            TrackingPacket predicted = {};
            REQUIRE(receiver.GetPredictedPose(predicted));
            CHECK(predicted.headPosition[0] > second.headPosition[0]);
            CHECK(predicted.headOrientation[1] > second.headOrientation[1]);
            for (float component : predicted.headPosition)
            {
                CHECK(std::isfinite(component));
            }
            for (float component : predicted.headOrientation)
            {
                CHECK(std::isfinite(component));
            }
        }
    }
}
