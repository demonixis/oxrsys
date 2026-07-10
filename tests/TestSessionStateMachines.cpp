// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "FocusEmulation.h"
#include "ProfileChangeDebounce.h"

#include <cstdint>
#include <string>

using oxrsys::EvaluateFocusEmulation;
using oxrsys::EvaluateProfileChangeDebounce;
using oxrsys::FocusEmulationState;
using oxrsys::ProfileChangeDebounceState;

namespace
{
constexpr int64_t kMs = 1'000'000;
constexpr int64_t kFocusLossDelayNs = 500 * kMs;    // matches Session::kFocusLossDelay
constexpr int64_t kProfileStableDelayNs = 1000 * kMs; // matches kProfileChangeStableDelay
} // namespace

TEST_CASE("Focus emulation: an unarmed machine never suppresses focus", "[session-fsm]")
{
    // No input has ever been seen active, so even a long focused silence (the connect
    // window, where controller flags lag) must not drop FOCUSED.
    const FocusEmulationState prev; // seenActive=false
    const auto decision = EvaluateFocusEmulation(prev, /*inputActive=*/false,
                                                 /*isFocused=*/true,
                                                 /*nowNs=*/10'000 * kMs, kFocusLossDelayNs);
    CHECK_FALSE(decision.suppressFocus);
    CHECK_FALSE(decision.state.seenActive);
    CHECK_FALSE(decision.state.suppressed);
}

TEST_CASE("Focus emulation: active input arms the machine and clears the ratchet", "[session-fsm]")
{
    FocusEmulationState prev;
    prev.suppressed = true; // was suppressed; input returning must restore next frame

    const auto decision = EvaluateFocusEmulation(prev, /*inputActive=*/true,
                                                 /*isFocused=*/false,
                                                 /*nowNs=*/2'000 * kMs, kFocusLossDelayNs);
    CHECK(decision.state.seenActive);
    CHECK_FALSE(decision.state.suppressed); // ratchet cleared -> caller restores FOCUSED
    CHECK(decision.state.lastActiveNs == 2'000 * kMs);
    CHECK_FALSE(decision.suppressFocus);
}

TEST_CASE("Focus emulation: 500ms of focused silence suppresses, but only when armed", "[session-fsm]")
{
    FocusEmulationState armed;
    armed.seenActive = true;
    armed.lastActiveNs = 1'000 * kMs;

    SECTION("Under the delay keeps focus")
    {
        const auto decision =
            EvaluateFocusEmulation(armed, false, true,
                                   /*nowNs=*/1'000 * kMs + kFocusLossDelayNs - 1, kFocusLossDelayNs);
        CHECK_FALSE(decision.suppressFocus);
        CHECK_FALSE(decision.state.suppressed);
    }

    SECTION("At the delay suppresses focus and trips the ratchet")
    {
        const auto decision =
            EvaluateFocusEmulation(armed, false, true,
                                   /*nowNs=*/1'000 * kMs + kFocusLossDelayNs, kFocusLossDelayNs);
        CHECK(decision.suppressFocus);
        CHECK(decision.state.suppressed);
    }

    SECTION("Silence while not FOCUSED never suppresses")
    {
        const auto decision =
            EvaluateFocusEmulation(armed, false, /*isFocused=*/false,
                                   /*nowNs=*/1'000 * kMs + 10 * kFocusLossDelayNs, kFocusLossDelayNs);
        CHECK_FALSE(decision.suppressFocus);
    }

    SECTION("An already-suppressed machine does not suppress again")
    {
        FocusEmulationState suppressed = armed;
        suppressed.suppressed = true;
        const auto decision =
            EvaluateFocusEmulation(suppressed, false, true,
                                   /*nowNs=*/1'000 * kMs + 10 * kFocusLossDelayNs, kFocusLossDelayNs);
        CHECK_FALSE(decision.suppressFocus);
    }
}

TEST_CASE("Profile debounce: startup empty signature announces nothing", "[session-fsm]")
{
    const ProfileChangeDebounceState prev; // lastNotified == "|"
    const auto decision = EvaluateProfileChangeDebounce(prev, "|", /*nowNs=*/0, kProfileStableDelayNs);
    CHECK_FALSE(decision.emit);
    CHECK(decision.state.pending.empty());
    CHECK(decision.state.lastNotified == "|");
}

TEST_CASE("Profile debounce: a stable signature change emits exactly one event", "[session-fsm]")
{
    const std::string sig = "oculus|oculus";
    ProfileChangeDebounceState state; // lastNotified == "|"

    // First sight starts the debounce clock; nothing emitted yet.
    auto d = EvaluateProfileChangeDebounce(state, sig, /*nowNs=*/0, kProfileStableDelayNs);
    state = d.state;
    CHECK_FALSE(d.emit);
    CHECK(state.pending == sig);

    // Still under 1s: no emit.
    d = EvaluateProfileChangeDebounce(state, sig, kProfileStableDelayNs - 1, kProfileStableDelayNs);
    state = d.state;
    CHECK_FALSE(d.emit);

    // Held steady for 1s: emit once and adopt the signature.
    d = EvaluateProfileChangeDebounce(state, sig, kProfileStableDelayNs, kProfileStableDelayNs);
    state = d.state;
    CHECK(d.emit);
    CHECK(d.emitted == sig);
    CHECK(state.lastNotified == sig);
    CHECK(state.pending.empty());

    // The same signature on later frames is already announced: no repeat event.
    d = EvaluateProfileChangeDebounce(state, sig, 5 * kProfileStableDelayNs, kProfileStableDelayNs);
    CHECK_FALSE(d.emit);
}

TEST_CASE("Profile debounce: an A->B->A flip within 1s emits nothing", "[session-fsm]")
{
    ProfileChangeDebounceState state;
    state.lastNotified = "A|A"; // A is the stable, already-announced baseline

    // Flip to B: starts debouncing, not yet announced.
    auto d = EvaluateProfileChangeDebounce(state, "B|B", /*nowNs=*/0, kProfileStableDelayNs);
    state = d.state;
    CHECK_FALSE(d.emit);
    CHECK(state.pending == "B|B");

    // Back to A well within 1s: the pending B is dropped and nothing fires.
    d = EvaluateProfileChangeDebounce(state, "A|A", 500 * kMs, kProfileStableDelayNs);
    state = d.state;
    CHECK_FALSE(d.emit);
    CHECK(state.pending.empty());
    CHECK(state.lastNotified == "A|A");

    // Even letting time run past 1s emits nothing: B never stabilized.
    d = EvaluateProfileChangeDebounce(state, "A|A", 5 * kProfileStableDelayNs, kProfileStableDelayNs);
    CHECK_FALSE(d.emit);
}

TEST_CASE("Profile debounce: a signature that changes before stabilizing restarts the clock",
          "[session-fsm]")
{
    ProfileChangeDebounceState state; // lastNotified "|"

    auto d = EvaluateProfileChangeDebounce(state, "B|B", /*nowNs=*/0, kProfileStableDelayNs);
    state = d.state;
    // New pending C at 800ms resets the clock to 800ms.
    d = EvaluateProfileChangeDebounce(state, "C|C", 800 * kMs, kProfileStableDelayNs);
    state = d.state;
    CHECK(state.pending == "C|C");
    CHECK(state.pendingSinceNs == 800 * kMs);

    // 900ms after the reset (< 1s): still no emit.
    d = EvaluateProfileChangeDebounce(state, "C|C", 1700 * kMs, kProfileStableDelayNs);
    state = d.state;
    CHECK_FALSE(d.emit);

    // 1s+ after the reset: C finally emits.
    d = EvaluateProfileChangeDebounce(state, "C|C", 1801 * kMs, kProfileStableDelayNs);
    CHECK(d.emit);
    CHECK(d.emitted == "C|C");
}

TEST_CASE("Profile debounce: keys on the app-visible signature only", "[session-fsm]")
{
    // Session builds the signature from SelectCurrentInteractionProfileForInstance —
    // the instance-filtered, app-visible profile (commit keying the debounce on the
    // app-visible profile). So a raw InputManager profile flip that leaves the visible
    // signature unchanged reaches this policy as the *same* signature, and no event
    // fires no matter how many frames pass.
    const std::string visible = "oculus|oculus";
    ProfileChangeDebounceState state;
    state.lastNotified = visible; // already announced

    for (int frame = 0; frame < 5; ++frame)
    {
        const auto d = EvaluateProfileChangeDebounce(state, visible,
                                                     static_cast<int64_t>(frame) * kProfileStableDelayNs,
                                                     kProfileStableDelayNs);
        state = d.state;
        CHECK_FALSE(d.emit);
        CHECK(state.pending.empty());
    }
}
