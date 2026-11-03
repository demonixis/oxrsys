// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <chrono>
#include <thread>

// pacing_thread = "realtime": which app thread Session::WaitFrame has given a time-constraint policy,
// and what to do on the next wait. Kept apart from the Mach calls so the decision can be tested on
// any platform.
namespace oxrsys::pacing_thread
{

// The promoted thread and the frame period its policy was set for. A default thread id means no
// thread is promoted.
struct Promoted
{
    std::thread::id thread{};
    std::chrono::nanoseconds period{0};
};

enum class Action
{
    Keep,    // nothing to change
    Promote, // save the caller's policy, then make it realtime
    Retune,  // same thread, new frame period: set the realtime policy again with that period
    Restore, // realtime pacing is off: give the promoted thread its saved policy back
    Move,    // another thread waits now: restore the promoted thread, then promote the caller
};

inline Action Decide(const Promoted& promoted, bool realtime, std::thread::id caller,
                     std::chrono::nanoseconds period)
{
    const bool anyPromoted = promoted.thread != std::thread::id{};
    if (!realtime)
    {
        return anyPromoted ? Action::Restore : Action::Keep;
    }
    if (!anyPromoted)
    {
        return Action::Promote;
    }
    if (promoted.thread != caller)
    {
        return Action::Move;
    }
    return promoted.period == period ? Action::Keep : Action::Retune;
}

} // namespace oxrsys::pacing_thread
