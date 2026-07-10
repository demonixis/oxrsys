// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

// Pure client-liveness decision, factored out of StreamingServer so the reconnect
// watchdog (abrupt UDP client kill -> resume broadcast) is unit-testable without
// a live session. StreamingServer holds the state in atomics and only evaluates
// it from SendFrame, i.e. while the app is still submitting frames.
namespace oxrsys
{

// Snapshot of what the watchdog remembers between checks: the tracking packet
// count last observed, and the clock reading when activity was last seen.
struct ClientLivenessState
{
    uint64_t lastCountSeen = 0;
    int64_t lastActivityNs = 0;
};

struct ClientLivenessDecision
{
    ClientLivenessState state; // stored back by the caller
    bool disconnect = false;   // client is considered gone
};

// Decides whether a connected client is still alive. A change in the tracking
// packet count since prev.lastCountSeen resets the activity clock to nowNs. Once
// the count has stalled, the client is declared disconnected when the last
// activity is older than timeoutNs — but only if activity was ever recorded
// (lastActivityNs != 0), so a client that has produced no packets yet is never
// disconnected on this path.
inline ClientLivenessDecision EvaluateClientLiveness(const ClientLivenessState& prev,
                                                     uint64_t currentCount, int64_t nowNs,
                                                     int64_t timeoutNs)
{
    ClientLivenessDecision decision;
    decision.state = prev;
    if (currentCount != prev.lastCountSeen)
    {
        decision.state.lastCountSeen = currentCount;
        decision.state.lastActivityNs = nowNs;
        return decision;
    }
    if (prev.lastActivityNs != 0 && nowNs - prev.lastActivityNs > timeoutNs)
    {
        decision.disconnect = true;
    }
    return decision;
}

} // namespace oxrsys
