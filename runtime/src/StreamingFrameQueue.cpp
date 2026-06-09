// SPDX-License-Identifier: MPL-2.0

#include "StreamingFrameQueue.h"

#include <utility>

StreamingFrameQueue::StreamingFrameQueue(ReleaseFrameCallback releaseFrame)
    : releaseFrame_(std::move(releaseFrame))
{
}

void StreamingFrameQueue::SetReleaseFrameCallback(ReleaseFrameCallback releaseFrame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    releaseFrame_ = std::move(releaseFrame);
}

void StreamingFrameQueue::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = false;
}

void StreamingFrameQueue::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    readyCv_.notify_all();
}

bool StreamingFrameQueue::PushLatest(StreamingFrame frame)
{
    frame.valid = true;

    StreamingFrame replacedFrame = {};
    bool replaced = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingFrame_.valid)
        {
            replacedFrame = std::move(pendingFrame_);
            replaced = true;
        }
        pendingFrame_ = std::move(frame);
    }

    ReleaseFrame(replacedFrame);
    readyCv_.notify_one();
    return replaced;
}

bool StreamingFrameQueue::WaitPop(const std::atomic<bool>& running, StreamingFrame& outFrame)
{
    std::unique_lock<std::mutex> lock(mutex_);
    readyCv_.wait(lock, [this, &running] {
        return !running.load() || stopped_ || pendingFrame_.valid;
    });

    if (!pendingFrame_.valid)
    {
        outFrame = {};
        return false;
    }

    outFrame = std::move(pendingFrame_);
    pendingFrame_ = {};
    return true;
}

void StreamingFrameQueue::Clear()
{
    StreamingFrame frame = {};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame = std::move(pendingFrame_);
        pendingFrame_ = {};
    }
    ReleaseFrame(frame);
}

void StreamingFrameQueue::ReleaseFrame(StreamingFrame& frame)
{
    if (!frame.valid)
    {
        return;
    }

    ReleaseFrameCallback releaseFrame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        releaseFrame = releaseFrame_;
    }
    if (releaseFrame)
    {
        releaseFrame(frame);
    }
    frame = {};
}
