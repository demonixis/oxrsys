// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

// Pure focus-emulation decision, factored out of Session so the FOCUSED<->VISIBLE
// menu-button state machine is unit-testable without a live streaming session.
// Session holds the state and only evaluates it once per submitted frame; the clock
// is injected as a nanosecond reading so tests drive transitions with synthetic time
// (no sleeps). See AdvanceSessionStateAfterFrameSubmission for the wiring rationale.
namespace oxrsys
{

// What the emulator remembers between frames. seenActive arms the machine only after
// input has been observed on the current connection (the connect window can lag input
// by >0.5s); suppressed is the ratchet that holds the session at VISIBLE until input
// returns; lastActiveNs is the clock reading when input was last seen active.
struct FocusEmulationState
{
    bool seenActive = false;
    bool suppressed = false;
    int64_t lastActiveNs = 0;
};

struct FocusEmulationDecision
{
    FocusEmulationState state; // stored back by the caller
    bool suppressFocus = false; // caller transitions FOCUSED->VISIBLE and returns
};

// Decides whether to suppress focus this frame. inputActive is "streaming AND a
// controller/hand is currently active" (composed by the caller). While input is
// active the machine arms, records the clock, and clears the ratchet so the caller's
// VISIBLE->FOCUSED transition restores focus next frame. Once armed and focused, a
// stretch of inactivity at least focusLossDelayNs long trips the ratchet and asks the
// caller to drop FOCUSED->VISIBLE. The unarmed machine (no input ever seen) never
// suppresses, so the connect window cannot drop focus.
inline FocusEmulationDecision EvaluateFocusEmulation(const FocusEmulationState& prev,
                                                     bool inputActive, bool isFocused,
                                                     int64_t nowNs, int64_t focusLossDelayNs)
{
    FocusEmulationDecision decision;
    decision.state = prev;
    if (inputActive)
    {
        decision.state.seenActive = true;
        decision.state.lastActiveNs = nowNs;
        decision.state.suppressed = false; // ratchet restores FOCUSED next frame
        return decision;
    }
    if (prev.seenActive && !prev.suppressed && isFocused &&
        nowNs - prev.lastActiveNs >= focusLossDelayNs)
    {
        decision.state.suppressed = true;
        decision.suppressFocus = true;
    }
    return decision;
}

} // namespace oxrsys
