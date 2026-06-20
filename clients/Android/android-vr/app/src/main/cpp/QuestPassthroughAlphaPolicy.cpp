// SPDX-License-Identifier: MPL-2.0

#include "QuestPassthroughAlphaPolicy.h"

namespace oxr::quest_passthrough
{

AlphaKeyDecision EvaluateAlphaKey(const AlphaKeyInput& input)
{
    if (!input.passthroughStreamingActive)
    {
        return {};
    }

    const bool usingFallback =
        !input.frameHasProtocolAlpha && !input.streamHasObservedProtocolAlpha;
    return {
        input.frameHasProtocolAlpha || usingFallback,
        usingFallback,
    };
}

} // namespace oxr::quest_passthrough
