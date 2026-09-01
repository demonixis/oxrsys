// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

// Tracks asynchronous ownership that must be gone before graphics objects may
// be destroyed. Stop() is idempotent and a timed-out Wait() is retryable.
class BoundedDrain
{
public:
    bool TryAcquire()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
        {
            return false;
        }
        ++active_;
        return true;
    }

    void Release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_ > 0)
            {
                --active_;
            }
        }
        condition_.notify_all();
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }

    bool StopAndWait(std::chrono::nanoseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        stopping_ = true;
        return condition_.wait_for(lock, timeout, [this] { return active_ == 0; });
    }

    bool IsStopping() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

    bool Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ != 0)
        {
            return false;
        }
        stopping_ = false;
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    size_t active_ = 0;
    bool stopping_ = false;
};
