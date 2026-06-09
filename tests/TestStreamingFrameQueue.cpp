// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "StreamingFrameQueue.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace
{

void* TestHandle(uintptr_t value)
{
    return reinterpret_cast<void*>(value);
}

FrameImageSource TestImageSource(uintptr_t value, std::vector<uintptr_t>& releasedHandles)
{
    FrameImageSource source = {};
    source.image = std::shared_ptr<void>(TestHandle(value), [&releasedHandles, value](void*) {
        releasedHandles.push_back(value);
    });
    return source;
}

FrameSource TestFrameSource(uintptr_t left, uintptr_t right, std::vector<uintptr_t>& releasedHandles)
{
    FrameSource source = {};
    source.left = TestImageSource(left, releasedHandles);
    source.right = TestImageSource(right, releasedHandles);
    return source;
}

} // namespace

TEST_CASE("StreamingFrameQueue replaces the pending frame and releases it", "[streaming]")
{
    uint32_t releasedFrameIndex = 0;
    std::vector<uintptr_t> releasedHandles;
    StreamingFrameQueue queue([&](StreamingFrame& frame) {
        releasedFrameIndex = frame.frameIndex;
    });
    queue.Start();

    StreamingFrame first = {};
    first.source = TestFrameSource(1, 2, releasedHandles);
    first.frameIndex = 7;
    CHECK(!queue.PushLatest(std::move(first)));

    StreamingFrame second = {};
    second.source = TestFrameSource(3, 4, releasedHandles);
    second.frameIndex = 8;
    CHECK(queue.PushLatest(std::move(second)));
    CHECK(releasedFrameIndex == 7);
    REQUIRE(releasedHandles.size() == 2);
    CHECK(releasedHandles[0] == 1);
    CHECK(releasedHandles[1] == 2);

    std::atomic<bool> running{true};
    StreamingFrame popped = {};
    CHECK(queue.WaitPop(running, popped));
    CHECK(popped.frameIndex == 8);
    CHECK(popped.source.left.GetImage() == TestHandle(3));
    CHECK(popped.source.right.GetImage() == TestHandle(4));
    CHECK(releasedHandles.size() == 2);

    popped = {};
    REQUIRE(releasedHandles.size() == 4);
    CHECK(releasedHandles[2] == 3);
    CHECK(releasedHandles[3] == 4);

    StreamingFrame third = {};
    third.source = TestFrameSource(5, 6, releasedHandles);
    third.frameIndex = 9;
    CHECK(!queue.PushLatest(std::move(third)));

    queue.Clear();
    REQUIRE(releasedHandles.size() == 6);
    CHECK(releasedHandles[4] == 5);
    CHECK(releasedHandles[5] == 6);
}
