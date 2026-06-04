// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "StreamingFrameQueue.h"

#include <atomic>
#include <cstdint>

namespace
{

void* TestHandle(uintptr_t value)
{
    return reinterpret_cast<void*>(value);
}

} // namespace

TEST_CASE("StreamingFrameQueue replaces the pending frame and releases it", "[streaming]")
{
    uint32_t releasedFrameIndex = 0;
    uint32_t releaseCount = 0;
    StreamingFrameQueue queue([&](StreamingFrame& frame) {
        releasedFrameIndex = frame.frameIndex;
        if (frame.source.leftTexture != nullptr)
        {
            releaseCount++;
        }
        if (frame.source.rightTexture != nullptr)
        {
            releaseCount++;
        }
    });
    queue.Start();

    StreamingFrame first = {};
    first.source.leftTexture = TestHandle(1);
    first.source.rightTexture = TestHandle(2);
    first.frameIndex = 7;
    CHECK(!queue.PushLatest(first));

    StreamingFrame second = {};
    second.source.leftTexture = TestHandle(3);
    second.source.rightTexture = TestHandle(4);
    second.frameIndex = 8;
    CHECK(queue.PushLatest(second));
    CHECK(releasedFrameIndex == 7);
    CHECK(releaseCount == 2);

    std::atomic<bool> running{true};
    StreamingFrame popped = {};
    CHECK(queue.WaitPop(running, popped));
    CHECK(popped.frameIndex == 8);
    CHECK(popped.source.leftTexture == TestHandle(3));
    CHECK(popped.source.rightTexture == TestHandle(4));

    queue.Clear();
    CHECK(releaseCount == 2);
}
