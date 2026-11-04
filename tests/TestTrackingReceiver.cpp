// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TrackingReceiver.h"

#include <atomic>
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

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet),
                          offsetof(TrackingPacket, rightControllerAimAngularVelocity) +
                              sizeof(packet.rightControllerAimAngularVelocity) - 1);
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

TEST_CASE("Tracking prediction limits head translation during rotation", "[tracking]")
{
    for (float angularSpeed : {0.0f, 4.0f})
    {
        for (float linearSpeed : {0.0f, 1.0f})
        {
            CAPTURE(angularSpeed, linearSpeed);
            TrackingReceiver receiver;
            receiver.SetPredictionHorizonMs(20.0f);
            TrackingPacket first = {};
            first.timestampNs = 1'000'000'000;
            first.headOrientation[3] = 1.0f;
            TrackingPacket second = first;
            second.timestampNs = 1'010'000'000;
            second.headPosition[0] = 0.02f;
            second.headLinearVelocity[0] = linearSpeed;
            second.headAngularVelocity[1] = angularSpeed;
            second.trackingFlags = TRACKING_FLAG_HEAD_LINEAR_VELOCITY_VALID |
                                   TRACKING_FLAG_HEAD_ANGULAR_VELOCITY_VALID;

            receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&first), sizeof(first));
            receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&second), sizeof(second));
            TrackingPacket predicted = {};
            REQUIRE(receiver.GetPredictedPose(predicted));
            const float horizonSeconds = angularSpeed == 0.0f ? 0.01f : 0.0015f;
            CHECK_THAT(predicted.headPosition[0],
                       WithinAbs(second.headPosition[0] + linearSpeed * horizonSeconds, 1e-6f));
        }
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
            CHECK_THAT(predicted.headPosition[0], WithinAbs(0.04f, 1e-6f));
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

TEST_CASE("Tracking receiver accepts UDP tracking only from the connected client", "[tracking]")
{
    TrackingReceiver receiver;
    sockaddr_in client = {};
    client.sin_family = AF_INET;
    client.sin_addr.s_addr = inet_addr("127.0.0.1");
    client.sin_port = htons(50000);
    sockaddr_in other = client;
    other.sin_addr.s_addr = inet_addr("127.0.0.2");

    TrackingPacket clientPacket = {};
    clientPacket.timestampNs = 1'000'000'000;
    clientPacket.headOrientation[3] = 1.0f;
    clientPacket.headPosition[0] = 1.0f;
    // Later than the client's packet, so the reorder guard alone would accept it.
    TrackingPacket otherPacket = clientPacket;
    otherPacket.timestampNs = 2'000'000'000;
    otherPacket.headPosition[0] = 2.0f;
    const auto* clientBytes = reinterpret_cast<const uint8_t*>(&clientPacket);
    const auto* otherBytes = reinterpret_cast<const uint8_t*>(&otherPacket);

    TrackingPacket received = {};
    receiver.ReceivePacket(clientBytes, sizeof(clientPacket), client);
    CHECK_FALSE(receiver.GetLatestPose(received));

    receiver.SetClientAddress(client.sin_addr.s_addr);
    receiver.ReceivePacket(clientBytes, sizeof(clientPacket), client);
    receiver.ReceivePacket(otherBytes, sizeof(otherPacket), other);
    REQUIRE(receiver.GetLatestPose(received));
    CHECK(received.headPosition[0] == 1.0f);
    CHECK(receiver.GetPacketCount() == 1);

    // The client keeps its place after a restart on a new port.
    clientPacket.timestampNs = 3'000'000'000;
    client.sin_port = htons(50001);
    receiver.ReceivePacket(clientBytes, sizeof(clientPacket), client);
    CHECK(receiver.GetPacketCount() == 2);

    receiver.SetClientAddress(0);
    clientPacket.timestampNs = 4'000'000'000;
    receiver.ReceivePacket(clientBytes, sizeof(clientPacket), client);
    CHECK(receiver.GetPacketCount() == 2);
}

TEST_CASE("Tracking receiver keeps no sample from a client replaced while it sends", "[tracking]")
{
    TrackingReceiver receiver;
    sockaddr_in previous = {};
    previous.sin_family = AF_INET;
    previous.sin_addr.s_addr = inet_addr("127.0.0.2");
    previous.sin_port = htons(50000);

    // A sample stored after the switch would shadow the next client's clock. The switch is
    // ordered like the server's, address first and then the history, while the replaced client
    // keeps sending; repeat it so a datagram is often in flight when the client changes.
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        CAPTURE(attempt);
        receiver.SetClientAddress(previous.sin_addr.s_addr);
        std::atomic<bool> sending{true};
        std::atomic<uint32_t> sent{0};
        std::thread sender([&] {
            TrackingPacket packet = {};
            packet.timestampNs = 500'000'000'000'000;
            packet.headOrientation[3] = 1.0f;
            while (sending.load())
            {
                ++packet.timestampNs;
                receiver.ReceivePacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet), previous);
                sent.fetch_add(1);
            }
        });
        while (sent.load() == 0)
        {
            std::this_thread::yield();
        }

        receiver.SetClientAddress(0);
        receiver.Reset();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        sending.store(false);
        sender.join();

        TrackingPacket latest = {};
        REQUIRE_FALSE(receiver.GetLatestPose(latest));
    }
}
