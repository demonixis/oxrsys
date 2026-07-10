// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "QuestPassthroughAlphaPolicy.h"

using namespace oxr::quest_passthrough;

TEST_CASE("Quest passthrough alpha policy requires active passthrough", "[quest][passthrough]")
{
    AlphaKeyDecision decision = EvaluateAlphaKey({
        false,
        false,
        false,
        false,
    });

    REQUIRE_FALSE(decision.useBlackKeyAlpha);
    REQUIRE_FALSE(decision.usingTransparentClearFallback);
}

TEST_CASE("Quest passthrough alpha policy uses only protocol alpha by default", "[quest][passthrough]")
{
    AlphaKeyDecision noProtocolAlpha = EvaluateAlphaKey({
        true,
        false,
        false,
        false,
    });
    REQUIRE_FALSE(noProtocolAlpha.useBlackKeyAlpha);
    REQUIRE_FALSE(noProtocolAlpha.usingTransparentClearFallback);

    AlphaKeyDecision firstProtocolAlphaFrame = EvaluateAlphaKey({
        true,
        true,
        false,
        false,
    });
    REQUIRE(firstProtocolAlphaFrame.useBlackKeyAlpha);
    REQUIRE_FALSE(firstProtocolAlphaFrame.usingTransparentClearFallback);

    AlphaKeyDecision laterOpaqueFrame = EvaluateAlphaKey({
        true,
        false,
        true,
        false,
    });
    REQUIRE_FALSE(laterOpaqueFrame.useBlackKeyAlpha);
    REQUIRE_FALSE(laterOpaqueFrame.usingTransparentClearFallback);

    AlphaKeyDecision laterProtocolAlphaFrame = EvaluateAlphaKey({
        true,
        true,
        true,
        false,
    });
    REQUIRE(laterProtocolAlphaFrame.useBlackKeyAlpha);
    REQUIRE_FALSE(laterProtocolAlphaFrame.usingTransparentClearFallback);
}

TEST_CASE("Quest passthrough alpha policy keeps transparent-clear fallback explicit", "[quest][passthrough]")
{
    AlphaKeyDecision fallback = EvaluateAlphaKey({
        true,
        false,
        false,
        true,
    });
    REQUIRE(fallback.useBlackKeyAlpha);
    REQUIRE(fallback.usingTransparentClearFallback);

    AlphaKeyDecision laterOpaqueFrame = EvaluateAlphaKey({
        true,
        false,
        true,
        true,
    });
    REQUIRE_FALSE(laterOpaqueFrame.useBlackKeyAlpha);
    REQUIRE_FALSE(laterOpaqueFrame.usingTransparentClearFallback);
}
