// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "ClientLiveness.h"

#include <cstdint>

using oxrsys::ClientLivenessState;
using oxrsys::EvaluateClientLiveness;

namespace
{
constexpr int64_t kTimeoutNs = 3'000'000'000; // 3s, matching kClientLivenessTimeoutNs
constexpr int64_t kMs = 1'000'000;
} // namespace

TEST_CASE("Client liveness: a new packet count resets the activity clock", "[client-liveness]")
{
    const ClientLivenessState prev{/*lastCountSeen=*/10, /*lastActivityNs=*/1'000 * kMs};
    // Count advanced and now is far past the timeout, but the advance itself is
    // proof of life: no disconnect, and the clock moves to now.
    const auto decision = EvaluateClientLiveness(prev, /*currentCount=*/11,
                                                 /*nowNs=*/10'000 * kMs, kTimeoutNs);
    CHECK_FALSE(decision.disconnect);
    CHECK(decision.state.lastCountSeen == 11);
    CHECK(decision.state.lastActivityNs == 10'000 * kMs);
}

TEST_CASE("Client liveness: silence under the timeout keeps the client", "[client-liveness]")
{
    const ClientLivenessState prev{/*lastCountSeen=*/10, /*lastActivityNs=*/1'000 * kMs};
    // Same count 2.5s later: under the 3s threshold, so the client survives and
    // the remembered state is unchanged.
    const auto decision = EvaluateClientLiveness(prev, /*currentCount=*/10,
                                                 /*nowNs=*/3'500 * kMs, kTimeoutNs);
    CHECK_FALSE(decision.disconnect);
    CHECK(decision.state.lastCountSeen == 10);
    CHECK(decision.state.lastActivityNs == 1'000 * kMs);
}

TEST_CASE("Client liveness: silence past the timeout disconnects", "[client-liveness]")
{
    const ClientLivenessState prev{/*lastCountSeen=*/10, /*lastActivityNs=*/1'000 * kMs};
    // Same count 3.001s later: strictly greater than the threshold -> disconnect.
    const auto decision = EvaluateClientLiveness(prev, /*currentCount=*/10,
                                                 /*nowNs=*/4'001 * kMs, kTimeoutNs);
    CHECK(decision.disconnect);

    // Exactly at the threshold is not "greater than", so it does not disconnect.
    const auto atThreshold = EvaluateClientLiveness(prev, /*currentCount=*/10,
                                                    /*nowNs=*/1'000 * kMs + kTimeoutNs, kTimeoutNs);
    CHECK_FALSE(atThreshold.disconnect);
}

TEST_CASE("Client liveness: a zero activity clock never disconnects", "[client-liveness]")
{
    const ClientLivenessState prev{/*lastCountSeen=*/0, /*lastActivityNs=*/0};
    // No activity has ever been recorded (count still 0); even far past the
    // timeout the client is never disconnected on this path.
    const auto decision = EvaluateClientLiveness(prev, /*currentCount=*/0,
                                                 /*nowNs=*/1'000'000 * kMs, kTimeoutNs);
    CHECK_FALSE(decision.disconnect);
    CHECK(decision.state.lastActivityNs == 0);
}

#include "KeyframeRequestLimiter.h"

TEST_CASE("Keyframe limiter: first request is accepted, spam is suppressed", "[client-liveness]")
{
    oxrsys::KeyframeRequestLimiter limiter;
    // Connect-time IDR spam: only the first request within the window forwards.
    CHECK(limiter.Accept(/*nowNs=*/600 * kMs));
    CHECK_FALSE(limiter.Accept(/*nowNs=*/700 * kMs));
    CHECK_FALSE(limiter.Accept(/*nowNs=*/600 * kMs + oxrsys::KeyframeRequestLimiter::kMinIntervalNs - 1));
}

TEST_CASE("Keyframe limiter: accepts again after the window and re-arms it", "[client-liveness]")
{
    oxrsys::KeyframeRequestLimiter limiter;
    CHECK(limiter.Accept(/*nowNs=*/600 * kMs));
    const int64_t second = 600 * kMs + oxrsys::KeyframeRequestLimiter::kMinIntervalNs;
    CHECK(limiter.Accept(second));
    // The window restarts from the accepted request, not the suppressed ones.
    CHECK_FALSE(limiter.Accept(second + 1));
    CHECK(limiter.Accept(second + oxrsys::KeyframeRequestLimiter::kMinIntervalNs));
}
