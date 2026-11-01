// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "EncodeBudget.h"

using namespace oxrsys::encode_budget;

namespace
{
constexpr double FramePeriodMs = 13.9;

uint32_t RunWindows(Controller& controller, double encodeMs, uint32_t windows)
{
    uint32_t level = controller.Level();
    for (uint32_t i = 0; i < windows; ++i)
    {
        level = controller.Update(encodeMs, FramePeriodMs);
    }
    return level;
}
} // namespace

TEST_CASE("Encode budget steps to stronger foveation after consecutive overloaded windows", "[streaming][encode_budget]")
{
    Controller controller(3);

    CHECK(controller.Update(20.0, FramePeriodMs) == 0);
    CHECK(controller.Update(20.0, FramePeriodMs) == 0);
    CHECK(controller.Update(20.0, FramePeriodMs) == 1);
    CHECK(RunWindows(controller, 70.0, 10) == 3);
}

TEST_CASE("Encode budget rides out an app that briefly slows encoding", "[streaming][encode_budget]")
{
    Controller controller(3);

    CHECK(RunWindows(controller, 15.5, 20) == 0);
    CHECK(controller.Update(25.0, FramePeriodMs) == 0);
    CHECK(controller.Update(25.0, FramePeriodMs) == 0);
    CHECK(controller.Update(12.0, FramePeriodMs) == 0);
    CHECK(controller.Update(25.0, FramePeriodMs) == 0);
}

TEST_CASE("Encode budget holds a level whose encode time is within the period", "[streaming][encode_budget]")
{
    Controller controller(3);

    CHECK(RunWindows(controller, 70.0, Controller::OverloadedWindows) == 1);
    CHECK(RunWindows(controller, 12.5, 20) == 1);
}

TEST_CASE("Encode budget starts again from the configured foveation after a reset", "[streaming][encode_budget]")
{
    Controller controller(4);

    CHECK(RunWindows(controller, 70.0, Controller::OverloadedWindows * 2) == 2);
    CHECK(RunWindows(controller, 5.0, 20) == 2);
    controller.Reset();
    CHECK(controller.Level() == 0);
}
