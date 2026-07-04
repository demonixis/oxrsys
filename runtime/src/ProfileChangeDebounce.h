// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>

// Pure interaction-profile-change debounce, factored out of Session so the 1s
// stability filter (and its A->B->A cancel) is unit-testable without a live session.
// Session builds the app-visible (instance-filtered) left|right signature and stores
// the state; the clock is injected as a nanosecond reading so tests drive the timing
// with synthetic time. See Session::MaybeEmitInteractionProfileChanged for why
// transient flaps must never reach the app (they churn Unity's input devices).
namespace oxrsys
{

// The signature last announced to the app, the signature currently waiting to
// stabilize, and when that pending signature was first seen. lastNotified starts at
// the both-hands-empty "|" signature so startup announces nothing until a real
// profile resolves.
struct ProfileChangeDebounceState
{
    std::string lastNotified = "|";
    std::string pending;
    int64_t pendingSinceNs = 0;
};

struct ProfileChangeDebounceDecision
{
    ProfileChangeDebounceState state; // stored back by the caller
    bool emit = false;                // caller emits XrEventDataInteractionProfileChanged
    std::string emitted;              // the announced signature (valid when emit)
};

// Decides whether the current signature should be announced. A signature equal to the
// last announced one drops any pending change (the A->B->A case: the profile returned
// before the pending change stabilized, so it must never fire late). A newly seen
// signature starts the debounce clock. A signature that has held steady for at least
// stableDelayNs is announced and becomes the new last-notified value.
inline ProfileChangeDebounceDecision EvaluateProfileChangeDebounce(
    const ProfileChangeDebounceState& prev, const std::string& sig, int64_t nowNs,
    int64_t stableDelayNs)
{
    ProfileChangeDebounceDecision decision;
    decision.state = prev;
    if (sig == prev.lastNotified)
    {
        decision.state.pending.clear();
        return decision;
    }
    if (sig != prev.pending)
    {
        decision.state.pending = sig;
        decision.state.pendingSinceNs = nowNs;
        return decision;
    }
    if (nowNs - prev.pendingSinceNs < stableDelayNs)
    {
        return decision;
    }
    decision.state.lastNotified = sig;
    decision.state.pending.clear();
    decision.emit = true;
    decision.emitted = sig;
    return decision;
}

} // namespace oxrsys
