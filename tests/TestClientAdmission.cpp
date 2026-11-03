// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "ClientAdmission.h"

#include <cstdint>

using oxrsys::ClientAdmissionState;
using oxrsys::EvaluateClientAdmission;

namespace
{
constexpr int64_t kLogIntervalNs = 5'000'000'000; // 5s, matching kClientAdmissionLogIntervalNs
constexpr int64_t kMs = 1'000'000;
} // namespace

TEST_CASE("Client admission: nothing connected accepts anyone", "[client-admission]")
{
    ClientAdmissionState state;
    const auto decision = EvaluateClientAdmission(state, /*clientConnected=*/false, "",
                                                  "192.0.2.20", 0, kLogIntervalNs);
    CHECK(decision.accept);
    CHECK_FALSE(decision.log);
}

TEST_CASE("Client admission: the connected address may reconnect (resend, restart)",
          "[client-admission]")
{
    ClientAdmissionState state;
    // A resent ClientConnect, or a restarted client on a new port, comes from the
    // same host. The port is not part of the identity.
    const auto decision = EvaluateClientAdmission(state, /*clientConnected=*/true,
                                                  "192.0.2.10", "192.0.2.10",
                                                  1'000 * kMs, kLogIntervalNs);
    CHECK(decision.accept);
    CHECK_FALSE(decision.log);
}

TEST_CASE("Client admission: a second address is refused and logged once", "[client-admission]")
{
    ClientAdmissionState state;
    const auto first = EvaluateClientAdmission(state, true, "192.0.2.10", "127.0.0.1",
                                               1'000 * kMs, kLogIntervalNs);
    CHECK_FALSE(first.accept);
    CHECK(first.log);
    // The simulator's auto-connect retries every second: refused again, not logged.
    const auto second = EvaluateClientAdmission(state, true, "192.0.2.10", "127.0.0.1",
                                                2'000 * kMs, kLogIntervalNs);
    CHECK_FALSE(second.accept);
    CHECK_FALSE(second.log);
    // Past the interval it is logged again, so a long-running refusal stays visible.
    const auto later = EvaluateClientAdmission(state, true, "192.0.2.10", "127.0.0.1",
                                               6'500 * kMs, kLogIntervalNs);
    CHECK_FALSE(later.accept);
    CHECK(later.log);
}

TEST_CASE("Client admission: a different refused address is logged immediately",
          "[client-admission]")
{
    ClientAdmissionState state;
    (void)EvaluateClientAdmission(state, true, "192.0.2.10", "127.0.0.1", 1'000 * kMs,
                                  kLogIntervalNs);
    const auto other = EvaluateClientAdmission(state, true, "192.0.2.10", "198.51.100.7",
                                               1'100 * kMs, kLogIntervalNs);
    CHECK_FALSE(other.accept);
    CHECK(other.log);
}

TEST_CASE("Client admission: a connected state with no address recorded accepts",
          "[client-admission]")
{
    // Defensive: state_ says Connected but the address was already cleared by a
    // disconnect in flight. Refusing here could strand the next client.
    ClientAdmissionState state;
    const auto decision = EvaluateClientAdmission(state, true, "", "127.0.0.1", 1'000 * kMs,
                                                  kLogIntervalNs);
    CHECK(decision.accept);
}

TEST_CASE("Client admission: a loopback WiFi client cannot take over a USB ADB session",
          "[client-admission]")
{
    // A USB ADB session records the placeholder address 127.0.0.1. The desktop simulator
    // connects over loopback from the same address, but over WiFi/UDP.
    ClientAdmissionState state;
    const auto first = EvaluateClientAdmission(state, true, "127.0.0.1", "127.0.0.1",
                                               1'000 * kMs, kLogIntervalNs,
                                               /*connectedUsesUsbAdb=*/true,
                                               /*candidateUsesUsbAdb=*/false);
    CHECK_FALSE(first.accept);
    CHECK(first.log);
    const auto retry = EvaluateClientAdmission(state, true, "127.0.0.1", "127.0.0.1",
                                               2'000 * kMs, kLogIntervalNs, true, false);
    CHECK_FALSE(retry.accept);
    CHECK_FALSE(retry.log);
}

TEST_CASE("Client admission: a USB ADB client cannot take over a WiFi session",
          "[client-admission]")
{
    ClientAdmissionState state;
    const auto decision = EvaluateClientAdmission(state, true, "192.0.2.10", "USB ADB",
                                                  1'000 * kMs, kLogIntervalNs,
                                                  /*connectedUsesUsbAdb=*/false,
                                                  /*candidateUsesUsbAdb=*/true);
    CHECK_FALSE(decision.accept);
    CHECK(decision.log);
}

TEST_CASE("Client admission: the USB ADB owner may reconnect", "[client-admission]")
{
    ClientAdmissionState state;
    const auto decision = EvaluateClientAdmission(state, true, "127.0.0.1", "USB ADB",
                                                  1'000 * kMs, kLogIntervalNs,
                                                  /*connectedUsesUsbAdb=*/true,
                                                  /*candidateUsesUsbAdb=*/true);
    CHECK(decision.accept);
}
