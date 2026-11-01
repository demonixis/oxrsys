// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>

// Pure admission decision for a WiFi ClientConnect, factored out of StreamingServer
// so the second-client case is unit-testable without a live session.
//
// The control thread accepts any ClientConnect while WiFi is enabled, whatever the
// server state. HandleClientConnect then resets the tracking history and rebuilds
// the encoder for the newcomer, so a second client that connects while a headset is
// streaming hijacks the session: the single TrackingReceiver interleaves two clocks
// and the rendered head pose jumps (seen as violent shaking in the headset when the
// desktop simulator auto-connected beside a real Steam Frame, 2026-10-17).
//
// Policy: while a client is connected, a ClientConnect from a DIFFERENT address is
// refused. A ClientConnect from the same address is still accepted, because that is
// how a client resends a lost ClientConnect and how a restarted client (new source
// port, same host) reconnects before the liveness watchdog has noticed the old one
// is gone. The port is deliberately not compared.
namespace oxrsys
{

struct ClientAdmissionDecision
{
    bool accept = true;
    // True when the refusal should be logged: the first refusal of a given address,
    // then at most once per logIntervalNs for the same address, so a retrying
    // client cannot flood the log.
    bool log = false;
};

// Remembered between decisions: which address was last refused and when it was
// last logged.
struct ClientAdmissionState
{
    std::string lastRefusedIp;
    int64_t lastRefusalLogNs = 0;
};

inline ClientAdmissionDecision EvaluateClientAdmission(ClientAdmissionState& state,
                                                       bool clientConnected,
                                                       const std::string& connectedIp,
                                                       const std::string& candidateIp,
                                                       int64_t nowNs,
                                                       int64_t logIntervalNs)
{
    ClientAdmissionDecision decision;
    if (!clientConnected || connectedIp.empty() || connectedIp == candidateIp)
    {
        return decision;
    }
    decision.accept = false;
    if (state.lastRefusedIp != candidateIp || state.lastRefusalLogNs == 0 ||
        nowNs - state.lastRefusalLogNs >= logIntervalNs)
    {
        decision.log = true;
        state.lastRefusedIp = candidateIp;
        state.lastRefusalLogNs = nowNs;
    }
    return decision;
}

} // namespace oxrsys
