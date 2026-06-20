// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "GraphicsTypes.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

struct StreamingFrame
{
    FrameSource source = {};
    uint32_t frameIndex = 0;
    int64_t timestampNs = 0;
    bool valid = false;
    bool alphaBlend = false;

    float headPosition[3] = {};
    float headOrientation[4] = {0, 0, 0, 1};
    bool hasPose = false;
};

class StreamingFrameQueue
{
public:
    using ReleaseFrameCallback = std::function<void(StreamingFrame&)>;

    explicit StreamingFrameQueue(ReleaseFrameCallback releaseFrame = {});

    void SetReleaseFrameCallback(ReleaseFrameCallback releaseFrame);
    void Start();
    void Stop();
    bool PushLatest(StreamingFrame frame);
    bool WaitPop(const std::atomic<bool>& running, StreamingFrame& outFrame);
    void Clear();

private:
    void ReleaseFrame(StreamingFrame& frame);

    mutable std::mutex mutex_;
    std::condition_variable readyCv_;
    StreamingFrame pendingFrame_ = {};
    ReleaseFrameCallback releaseFrame_;
    bool stopped_ = true;
};
