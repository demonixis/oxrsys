// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "SpaceVelocity.h"
#include <limits>

using Catch::Matchers::WithinAbs;
using oxrsys::space_velocity::AtOffset;
using oxrsys::space_velocity::Relative;

namespace
{

XrSpaceVelocity Velocity(XrVector3f linear, XrVector3f angular)
{
    XrSpaceVelocity velocity = {XR_TYPE_SPACE_VELOCITY};
    velocity.velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
    velocity.linearVelocity = linear;
    velocity.angularVelocity = angular;
    return velocity;
}

} // namespace

TEST_CASE("Space velocity includes the rotation of an offset origin", "[spaces][velocity]")
{
    const XrSpaceVelocity velocity = Velocity({1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 2.0f});
    const XrSpaceVelocity result = AtOffset(velocity, {0.5f, 0.0f, 0.0f});
    CHECK(result.velocityFlags == velocity.velocityFlags);
    CHECK_THAT(result.linearVelocity.x, WithinAbs(1.0f, 0.0001f));
    CHECK_THAT(result.linearVelocity.y, WithinAbs(3.0f, 0.0001f));
    CHECK_THAT(result.linearVelocity.z, WithinAbs(3.0f, 0.0001f));
    CHECK_THAT(result.angularVelocity.z, WithinAbs(2.0f, 0.0001f));
}

TEST_CASE("Space velocity accounts for translating and rotating bases", "[spaces][velocity]")
{
    const XrSpaceVelocity velocity = Velocity({3.0f, 4.0f, 5.0f}, {0.0f, 0.0f, 3.0f});
    const XrSpaceVelocity baseVelocity = Velocity({1.0f, 1.0f, 2.0f}, {0.0f, 0.0f, 1.0f});
    const glm::quat baseOrientation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const XrSpaceVelocity result = Relative(velocity, baseVelocity, baseOrientation, {2.0f, 0.0f, 0.0f});
    CHECK(result.velocityFlags == velocity.velocityFlags);
    // World relative derivative is (3,4,5)-(1,1,2)-(0,2,0) = (2,1,3).
    // Expressed in the +90 degree Z base, this becomes (1,-2,3).
    CHECK_THAT(result.linearVelocity.x, WithinAbs(1.0f, 0.0001f));
    CHECK_THAT(result.linearVelocity.y, WithinAbs(-2.0f, 0.0001f));
    CHECK_THAT(result.linearVelocity.z, WithinAbs(3.0f, 0.0001f));
    CHECK_THAT(result.angularVelocity.z, WithinAbs(2.0f, 0.0001f));
}

TEST_CASE("World-stationary points move relative to a rotating base", "[spaces][velocity]")
{
    const XrSpaceVelocity stationary = Velocity({}, {});
    const XrSpaceVelocity baseVelocity = Velocity({}, {0.0f, 0.0f, 2.0f});
    const XrSpaceVelocity result = Relative(stationary, baseVelocity, {1.0f, 0.0f, 0.0f, 0.0f},
                                            {3.0f, 0.0f, 0.0f});
    CHECK(result.velocityFlags == stationary.velocityFlags);
    CHECK_THAT(result.linearVelocity.y, WithinAbs(-6.0f, 0.0001f));
    CHECK_THAT(result.angularVelocity.z, WithinAbs(-2.0f, 0.0001f));
}

TEST_CASE("Rigid offsets remain stationary in their own moving frame", "[spaces][velocity]")
{
    const XrSpaceVelocity baseVelocity = Velocity({1.0f, 2.0f, 3.0f}, {0.0f, 2.0f, 0.0f});
    const glm::vec3 offset(0.0f, 0.0f, -0.5f);
    const XrSpaceVelocity velocity = AtOffset(baseVelocity, offset);
    const XrSpaceVelocity result = Relative(velocity, baseVelocity, {1.0f, 0.0f, 0.0f, 0.0f}, offset);
    CHECK(result.velocityFlags == baseVelocity.velocityFlags);
    CHECK_THAT(result.linearVelocity.x, WithinAbs(0.0f, 0.0001f));
    CHECK_THAT(result.linearVelocity.y, WithinAbs(0.0f, 0.0001f));
    CHECK_THAT(result.linearVelocity.z, WithinAbs(0.0f, 0.0001f));
    CHECK_THAT(result.angularVelocity.y, WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("Space velocity validity follows the measurements required by the transform", "[spaces][velocity]")
{
    XrSpaceVelocity velocity = Velocity({1.0f, 0.0f, 0.0f}, {});
    const XrSpaceVelocity stationary = Velocity({}, {});
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);

    SECTION("An offset requires angular velocity to locate its linear velocity")
    {
        velocity.velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
        CHECK(AtOffset(velocity, {}).velocityFlags == XR_SPACE_VELOCITY_LINEAR_VALID_BIT);
        CHECK(AtOffset(velocity, {1.0f, 0.0f, 0.0f}).velocityFlags == 0);
    }
    SECTION("A moving base without angular velocity cannot resolve a displaced origin")
    {
        velocity.velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
        CHECK(Relative(stationary, velocity, identity, {}).velocityFlags == XR_SPACE_VELOCITY_LINEAR_VALID_BIT);
        CHECK(Relative(stationary, velocity, identity, {1.0f, 0.0f, 0.0f}).velocityFlags == 0);
    }
    SECTION("Missing linear velocity does not discard a measured angular velocity")
    {
        velocity.velocityFlags = XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
        CHECK(Relative(velocity, stationary, identity, {}).velocityFlags == XR_SPACE_VELOCITY_ANGULAR_VALID_BIT);
    }
    SECTION("Non-finite inputs never become valid output")
    {
        velocity.linearVelocity.x = std::numeric_limits<float>::quiet_NaN();
        CHECK(AtOffset(velocity, {}).velocityFlags == XR_SPACE_VELOCITY_ANGULAR_VALID_BIT);
        CHECK(Relative(velocity, stationary, identity, {}).velocityFlags == XR_SPACE_VELOCITY_ANGULAR_VALID_BIT);
        CHECK(Relative(stationary, stationary, glm::quat(0.0f, 0.0f, 0.0f, 0.0f), {}).velocityFlags == 0);
    }
}
