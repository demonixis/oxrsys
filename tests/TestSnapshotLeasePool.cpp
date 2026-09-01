// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "SnapshotLeasePool.h"

#include <chrono>

TEST_CASE("Snapshot lease slots are bounded and reusable", "[graphics][snapshot]")
{
    SnapshotLeasePool pool(2);
    auto first = pool.TryAcquire([](size_t) { return true; });
    auto second = pool.TryAcquire([](size_t) { return true; });

    REQUIRE(first);
    REQUIRE(second);
    CHECK(first.index != second.index);
    CHECK_FALSE(pool.TryAcquire([](size_t) { return true; }));

    const size_t releasedIndex = first.index;
    first.lifetime.reset();
    auto reused = pool.TryAcquire([](size_t) { return true; });
    REQUIRE(reused);
    CHECK(reused.index == releasedIndex);
}

TEST_CASE("Snapshot lease drain is bounded and stops new work", "[graphics][snapshot]")
{
    using namespace std::chrono_literals;

    SnapshotLeasePool pool(1);
    auto lease = pool.TryAcquire([](size_t) { return true; });
    REQUIRE(lease);

    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(pool.StopAndWaitForLeases(2ms));
    CHECK(std::chrono::steady_clock::now() - start < 250ms);
    CHECK_FALSE(pool.TryAcquire([](size_t) { return true; }));

    lease.lifetime.reset();
    CHECK(pool.StopAndWaitForLeases(20ms));
}
