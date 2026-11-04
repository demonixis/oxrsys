// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "TimespecConversion.h"

#include <cstdint>
#include <ctime>
#include <limits>

using oxrsys::MonotonicTimespecToXrTime;
using oxrsys::XrTimeToMonotonicTimespec;

namespace
{
constexpr int64_t kBaseNs = 5'000'000'000LL + 250; // 5 s plus a sub-second remainder

struct timespec MakeTimespec(time_t sec, long nsec)
{
    struct timespec ts{};
    ts.tv_sec = sec;
    ts.tv_nsec = nsec;
    return ts;
}
} // namespace

TEST_CASE("Timespec conversion rejects zero and negative XrTime", "[timespec]")
{
    struct timespec ts = MakeTimespec(7, 7);
    CHECK(XrTimeToMonotonicTimespec(0, kBaseNs, ts) == XR_ERROR_TIME_INVALID);
    CHECK(XrTimeToMonotonicTimespec(-1, kBaseNs, ts) == XR_ERROR_TIME_INVALID);
    CHECK(XrTimeToMonotonicTimespec(std::numeric_limits<XrTime>::min(), kBaseNs, ts) ==
          XR_ERROR_TIME_INVALID);
    // A rejected call leaves the output untouched.
    CHECK(ts.tv_sec == 7);
    CHECK(ts.tv_nsec == 7);
}

TEST_CASE("Timespec conversion rejects XrTime that overflows CLOCK_MONOTONIC", "[timespec]")
{
    struct timespec ts{};
    CHECK(XrTimeToMonotonicTimespec(std::numeric_limits<XrTime>::max(), kBaseNs, ts) ==
          XR_ERROR_TIME_INVALID);
    const XrTime largestValid = std::numeric_limits<int64_t>::max() - kBaseNs;
    CHECK(XrTimeToMonotonicTimespec(largestValid, kBaseNs, ts) == XR_SUCCESS);
    CHECK(XrTimeToMonotonicTimespec(largestValid + 1, kBaseNs, ts) == XR_ERROR_TIME_INVALID);
}

TEST_CASE("Timespec conversion rejects timespecs at or before the time base", "[timespec]")
{
    XrTime time = 42;
    // Exactly the base maps to XrTime 0, which is not a valid time.
    CHECK(MonotonicTimespecToXrTime(MakeTimespec(5, 250), kBaseNs, time) == XR_ERROR_TIME_INVALID);
    CHECK(MonotonicTimespecToXrTime(MakeTimespec(5, 249), kBaseNs, time) == XR_ERROR_TIME_INVALID);
    CHECK(MonotonicTimespecToXrTime(MakeTimespec(-1, 0), kBaseNs, time) == XR_ERROR_TIME_INVALID);
    CHECK(MonotonicTimespecToXrTime(MakeTimespec(std::numeric_limits<time_t>::max(), 0), kBaseNs,
                                    time) == XR_ERROR_TIME_INVALID);
    CHECK(time == 42);

    CHECK(MonotonicTimespecToXrTime(MakeTimespec(5, 251), kBaseNs, time) == XR_SUCCESS);
    CHECK(time == 1);
}

TEST_CASE("Timespec conversion rejects malformed tv_nsec", "[timespec]")
{
    XrTime time = 0;
    CHECK(MonotonicTimespecToXrTime(MakeTimespec(100, -1), kBaseNs, time) ==
          XR_ERROR_VALIDATION_FAILURE);
    CHECK(MonotonicTimespecToXrTime(MakeTimespec(100, 1'000'000'000L), kBaseNs, time) ==
          XR_ERROR_VALIDATION_FAILURE);
}

TEST_CASE("Timespec conversion round-trips valid times losslessly", "[timespec]")
{
    const struct timespec original = MakeTimespec(1'234'567, 999'999'999L);
    XrTime time = 0;
    REQUIRE(MonotonicTimespecToXrTime(original, kBaseNs, time) == XR_SUCCESS);
    CHECK(time == 1'234'567LL * 1'000'000'000LL + 999'999'999LL - kBaseNs);

    struct timespec roundTrip{};
    REQUIRE(XrTimeToMonotonicTimespec(time, kBaseNs, roundTrip) == XR_SUCCESS);
    CHECK(roundTrip.tv_sec == original.tv_sec);
    CHECK(roundTrip.tv_nsec == original.tv_nsec);
}
