// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "QuestShellInteraction.h"

using namespace oxr::quest_shell;

namespace
{

PanelLayout TestPanel(bool passthroughSupported = true)
{
    return MakeDefaultPanelLayout(
        {0.0f, 1.5f, -1.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        passthroughSupported);
}

Ray RayAt(float localX, float localY)
{
    const Vec3 origin = {localX, 1.5f + localY, 0.0f};
    return {origin, {0.0f, 0.0f, -1.0f}};
}

} // namespace

TEST_CASE("Quest shell controller ray hits reset and passthrough buttons", "[quest][shell]")
{
    const PanelLayout panel = TestPanel();

    ButtonHit reset = RaycastButtons(panel, RayAt(-0.19f, -0.155f));
    REQUIRE(reset.id == ButtonId::Reset);
    REQUIRE(reset.enabled);

    ButtonHit passthrough = RaycastButtons(panel, RayAt(0.19f, -0.155f));
    REQUIRE(passthrough.id == ButtonId::TogglePassthrough);
    REQUIRE(passthrough.enabled);

    ButtonHit miss = RaycastButtons(panel, RayAt(0.0f, 0.16f));
    REQUIRE(miss.id == ButtonId::None);
}

TEST_CASE("Quest shell controller trigger clicks once until release", "[quest][shell]")
{
    const PanelLayout panel = TestPanel();
    ControllerClickState state = {};
    const Ray resetRay = RayAt(-0.19f, -0.155f);

    ControllerResult first = UpdateController(panel, resetRay, 0.8f, &state);
    REQUIRE(first.hover.id == ButtonId::Reset);
    REQUIRE(first.click == ButtonId::Reset);

    ControllerResult held = UpdateController(panel, resetRay, 0.9f, &state);
    REQUIRE(held.hover.id == ButtonId::Reset);
    REQUIRE(held.click == ButtonId::None);

    ControllerResult partiallyReleased = UpdateController(panel, resetRay, 0.4f, &state);
    REQUIRE(partiallyReleased.click == ButtonId::None);

    ControllerResult released = UpdateController(panel, resetRay, 0.1f, &state);
    REQUIRE(released.click == ButtonId::None);

    ControllerResult second = UpdateController(panel, resetRay, 0.8f, &state);
    REQUIRE(second.click == ButtonId::Reset);
}

TEST_CASE("Quest shell hand pinch laser clicks once until release", "[quest][shell]")
{
    const PanelLayout panel = TestPanel();
    ControllerClickState state = {};
    const Ray resetRay = RayAt(-0.19f, -0.155f);

    ControllerResult first = UpdateController(panel, resetRay, 0.82f, &state, 0.75f, 0.25f);
    REQUIRE(first.hover.id == ButtonId::Reset);
    REQUIRE(first.click == ButtonId::Reset);

    ControllerResult held = UpdateController(panel, resetRay, 0.95f, &state, 0.75f, 0.25f);
    REQUIRE(held.hover.id == ButtonId::Reset);
    REQUIRE(held.click == ButtonId::None);

    ControllerResult partialRelease = UpdateController(panel, resetRay, 0.35f, &state, 0.75f, 0.25f);
    REQUIRE(partialRelease.click == ButtonId::None);

    ControllerResult release = UpdateController(panel, resetRay, 0.10f, &state, 0.75f, 0.25f);
    REQUIRE(release.click == ButtonId::None);

    ControllerResult second = UpdateController(panel, resetRay, 0.82f, &state, 0.75f, 0.25f);
    REQUIRE(second.click == ButtonId::Reset);
}

TEST_CASE("Quest shell disabled passthrough button cannot click", "[quest][shell]")
{
    const PanelLayout panel = TestPanel(false);
    ControllerClickState controllerState = {};
    ControllerResult controller = UpdateController(panel, RayAt(0.19f, -0.155f), 0.8f,
                                                   &controllerState);
    REQUIRE(controller.hover.id == ButtonId::TogglePassthrough);
    REQUIRE_FALSE(controller.hover.enabled);
    REQUIRE(controller.click == ButtonId::None);

    ControllerClickState pinchState = {};
    ControllerResult pinch = UpdateController(panel, RayAt(0.19f, -0.155f), 0.9f,
                                              &pinchState, 0.75f, 0.25f);
    REQUIRE(pinch.hover.id == ButtonId::TogglePassthrough);
    REQUIRE_FALSE(pinch.hover.enabled);
    REQUIRE(pinch.click == ButtonId::None);
}
