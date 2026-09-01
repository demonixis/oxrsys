// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "BoundedDrain.h"

#include <chrono>

TEST_CASE("Bounded async drain times out without losing retry state", "[lifecycle][drain]")
{
    using namespace std::chrono_literals;
    BoundedDrain drain;
    REQUIRE(drain.TryAcquire());
    CHECK_FALSE(drain.StopAndWait(1ms));
    CHECK(drain.IsStopping());
    CHECK_FALSE(drain.TryAcquire());

    drain.Release();
    CHECK(drain.StopAndWait(10ms));
    REQUIRE(drain.Reset());
    CHECK(drain.TryAcquire());
    drain.Release();
}
