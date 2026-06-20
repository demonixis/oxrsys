// SPDX-License-Identifier: MPL-2.0

#pragma once

namespace oxr::quest_passthrough
{

struct AlphaKeyInput
{
    bool passthroughStreamingActive = false;
    bool frameHasProtocolAlpha = false;
    bool streamHasObservedProtocolAlpha = false;
};

struct AlphaKeyDecision
{
    bool useBlackKeyAlpha = false;
    bool usingTransparentClearFallback = false;
};

AlphaKeyDecision EvaluateAlphaKey(const AlphaKeyInput& input);

} // namespace oxr::quest_passthrough
