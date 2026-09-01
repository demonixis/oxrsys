// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// Small backend-agnostic lease pool used by release-time graphics snapshots.
// A lease only protects a preallocated slot from reuse. Backend synchronization
// (for example a Vulkan fence) remains the caller's responsibility.
class SnapshotLeasePool
{
public:
    struct Lease
    {
        size_t index = 0;
        std::shared_ptr<void> lifetime = {};

        explicit operator bool() const { return lifetime != nullptr; }
    };

    explicit SnapshotLeasePool(size_t capacity)
        : state_(std::make_shared<State>(capacity))
    {
    }

    SnapshotLeasePool(const SnapshotLeasePool&) = delete;
    SnapshotLeasePool& operator=(const SnapshotLeasePool&) = delete;

    template <typename IsReusable>
    Lease TryAcquire(IsReusable&& isReusable)
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->stopping)
        {
            return {};
        }

        for (size_t index = 0; index < state_->inUse.size(); ++index)
        {
            if (state_->inUse[index] || !isReusable(index))
            {
                continue;
            }

            state_->inUse[index] = true;
            std::shared_ptr<State> state = state_;
            return {
                index,
                std::shared_ptr<void>(state.get(), [state, index](void*) {
                    {
                        std::lock_guard<std::mutex> releaseLock(state->mutex);
                        state->inUse[index] = false;
                    }
                    state->condition.notify_all();
                }),
            };
        }
        return {};
    }

    bool StopAndWaitForLeases(std::chrono::nanoseconds timeout)
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->stopping = true;
        return state_->condition.wait_for(lock, timeout, [this] {
            return std::none_of(state_->inUse.begin(), state_->inUse.end(),
                                [](bool inUse) { return inUse; });
        });
    }

    size_t Capacity() const
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->inUse.size();
    }

private:
    struct State
    {
        explicit State(size_t capacity)
            : inUse(capacity, false)
        {
        }

        std::mutex mutex;
        std::condition_variable condition;
        std::vector<bool> inUse;
        bool stopping = false;
    };

    std::shared_ptr<State> state_;
};
