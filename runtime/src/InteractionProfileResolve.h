// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>

#include "InputManager.h"

class Instance;

// Resolve the app-visible interaction profile for a hand: the candidate list is
// filtered through the instance version / enabled extensions (the same
// IsKnownInteractionProfilePath rules), falling back to oculus/touch. This is
// exactly the value xrGetCurrentInteractionProfile returns, and it can differ
// from the raw InputManager profile (e.g. ext/hand_interaction_ext when the app
// never enabled XR_EXT_hand_interaction). Shared by the getter and the Session
// interaction-profile-changed debounce so the two can never diverge.
std::string SelectCurrentInteractionProfileForInstance(
    const Instance* instance, const InputManager& inputManager, InputManager::Hand hand);
