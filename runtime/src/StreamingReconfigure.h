// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

namespace oxrsys::streaming_reconfigure
{

enum class State : uint8_t
{
    Idle,
    Pending,
    Acked,
    TimedOut,
};

enum class TickAction : uint8_t
{
    None,
    Retry,
    Timeout,
};

class PendingState
{
public:
    void Reset()
    {
        state_ = State::Idle;
        sequence_ = 0;
        retryCount_ = 0;
        lastSendNs_ = 0;
    }

    void Begin(uint32_t sequence, int64_t nowNs)
    {
        state_ = State::Pending;
        sequence_ = sequence;
        retryCount_ = 0;
        lastSendNs_ = nowNs;
    }

    TickAction Tick(int64_t nowNs, int64_t timeoutNs, uint32_t maxRetries)
    {
        if (state_ != State::Pending || sequence_ == 0)
        {
            return TickAction::None;
        }
        if (nowNs - lastSendNs_ < timeoutNs)
        {
            return TickAction::None;
        }
        if (retryCount_ < maxRetries)
        {
            retryCount_++;
            lastSendNs_ = nowNs;
            return TickAction::Retry;
        }

        state_ = State::TimedOut;
        sequence_ = 0;
        retryCount_ = 0;
        lastSendNs_ = 0;
        return TickAction::Timeout;
    }

    bool AcceptAck(uint32_t sequence, bool accepted)
    {
        if (state_ != State::Pending || sequence_ == 0 || sequence != sequence_)
        {
            return false;
        }
        state_ = accepted ? State::Acked : State::Idle;
        sequence_ = accepted ? sequence : 0;
        if (!accepted)
        {
            retryCount_ = 0;
            lastSendNs_ = 0;
        }
        return accepted;
    }

    void CompleteAcked()
    {
        if (state_ == State::Acked)
        {
            Reset();
        }
    }

    State state() const { return state_; }
    uint32_t sequence() const { return sequence_; }
    uint32_t retryCount() const { return retryCount_; }

private:
    State state_ = State::Idle;
    uint32_t sequence_ = 0;
    uint32_t retryCount_ = 0;
    int64_t lastSendNs_ = 0;
};

inline bool AllowsLiveReconfigure(bool reliableControlTransport, bool clientSupportsReconfigure)
{
    return reliableControlTransport && clientSupportsReconfigure;
}

} // namespace oxrsys::streaming_reconfigure
