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
    });

    REQUIRE_FALSE(decision.useBlackKeyAlpha);
    REQUIRE_FALSE(decision.usingTransparentClearFallback);
}

TEST_CASE("Quest passthrough alpha policy falls back until protocol alpha is observed", "[quest][passthrough]")
{
    AlphaKeyDecision noAlphaYet = EvaluateAlphaKey({
        true,
        false,
        false,
    });
    REQUIRE(noAlphaYet.useBlackKeyAlpha);
    REQUIRE(noAlphaYet.usingTransparentClearFallback);

    AlphaKeyDecision firstProtocolAlphaFrame = EvaluateAlphaKey({
        true,
        true,
        false,
    });
    REQUIRE(firstProtocolAlphaFrame.useBlackKeyAlpha);
    REQUIRE_FALSE(firstProtocolAlphaFrame.usingTransparentClearFallback);

    AlphaKeyDecision laterOpaqueFrame = EvaluateAlphaKey({
        true,
        false,
        true,
    });
    REQUIRE_FALSE(laterOpaqueFrame.useBlackKeyAlpha);
    REQUIRE_FALSE(laterOpaqueFrame.usingTransparentClearFallback);

    AlphaKeyDecision laterProtocolAlphaFrame = EvaluateAlphaKey({
        true,
        true,
        true,
    });
    REQUIRE(laterProtocolAlphaFrame.useBlackKeyAlpha);
    REQUIRE_FALSE(laterProtocolAlphaFrame.usingTransparentClearFallback);
}
