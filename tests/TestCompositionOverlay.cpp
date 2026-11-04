// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "CompositionOverlay.h"

#include <cmath>

using namespace oxrsys::runtime;
using Catch::Approx;

namespace
{

constexpr float kPi = 3.14159265358979f;

XrPosef Pose(float x, float y, float z, float yawRadians = 0.0f)
{
    // Rotation about +Y (counter-clockwise seen from above: +yaw turns the view to the left).
    XrPosef pose = {};
    pose.orientation = {0.0f, std::sin(yawRadians / 2.0f), 0.0f, std::cos(yawRadians / 2.0f)};
    pose.position = {x, y, z};
    return pose;
}

XrFovf SymmetricFov(float halfAngle)
{
    return {-halfAngle, halfAngle, halfAngle, -halfAngle};
}

// Normalized device coordinates of a layer-local point; w is returned for clip checks.
glm::vec4 Project(const glm::mat4& clipFromLayer, const glm::vec3& local)
{
    return clipFromLayer * glm::vec4(local, 1.0f);
}

glm::vec2 Ndc(const glm::vec4& clip)
{
    return glm::vec2(clip.x / clip.w, clip.y / clip.w);
}

} // namespace

TEST_CASE("A quad corner lands where its angle says, for an eye at the origin", "[runtime][overlay]")
{
    // 2 x 2 m quad 2 m ahead: its top-right corner is 1 m right and 1 m up at 2 m,
    // tan = 0.5 on both axes; with a 45 degree half-fov (tan = 1) that is NDC 0.5.
    const glm::mat4 m = OverlayClipFromLayer(Pose(0, 0, 0), SymmetricFov(kPi / 4), Pose(0, 0, 0), Pose(0, 0, -2));
    const glm::vec2 corner = Ndc(Project(m, OverlayQuadPoint(2.0f, 2.0f, 1.0f, 0.0f)));
    REQUIRE(corner.x == Approx(0.5f).margin(1e-5));
    REQUIRE(corner.y == Approx(0.5f).margin(1e-5)); // t = 0 is the image's top row, NDC +Y
    const glm::vec2 centre = Ndc(Project(m, OverlayQuadPoint(2.0f, 2.0f, 0.5f, 0.5f)));
    REQUIRE(centre.x == Approx(0.0f).margin(1e-5));
    REQUIRE(centre.y == Approx(0.0f).margin(1e-5));
}

TEST_CASE("A turned eye sees a quad placed off to its side", "[runtime][overlay]")
{
    // Yaw +90 degrees: the eye looks down world -X and its right hand points down world -Z.
    // A quad 2 m out on -X is centred; a point 1 m toward -Z from it is to the right, tan 0.5.
    const glm::mat4 m = OverlayClipFromLayer(Pose(0, 0, 0, kPi / 2), SymmetricFov(kPi / 4),
                                             Pose(0, 0, 0), Pose(-2, 0, 0, kPi / 2));
    const glm::vec2 centre = Ndc(Project(m, glm::vec3(0, 0, 0)));
    REQUIRE(centre.x == Approx(0.0f).margin(1e-5));
    REQUIRE(centre.y == Approx(0.0f).margin(1e-5));

    const glm::mat4 world = OverlayClipFromLayer(Pose(0, 0, 0, kPi / 2), SymmetricFov(kPi / 4),
                                                 Pose(0, 0, 0), Pose(0, 0, 0));
    const glm::vec2 right = Ndc(Project(world, glm::vec3(-2, 0, -1)));
    REQUIRE(right.x == Approx(0.5f).margin(1e-5));
    REQUIRE(right.y == Approx(0.0f).margin(1e-5));
}

TEST_CASE("The layer's space offset and the eye's own offset both move the image", "[runtime][overlay]")
{
    // Layer space sits 1 m above the projection space; the quad 2 m ahead in it: up tan 0.5.
    const glm::mat4 raised = OverlayClipFromLayer(Pose(0, 0, 0), SymmetricFov(kPi / 4),
                                                  Pose(0, 1, 0), Pose(0, 0, -2));
    REQUIRE(Ndc(Project(raised, glm::vec3(0, 0, 0))).y == Approx(0.5f).margin(1e-5));

    // A right eye 32 mm to the right sees a point straight ahead of the head 32 mm to its left
    // at 1 m: tan -0.032.
    const glm::mat4 rightEye = OverlayClipFromLayer(Pose(0.032f, 0, 0), SymmetricFov(kPi / 4),
                                                    Pose(0, 0, 0), Pose(0, 0, -1));
    REQUIRE(Ndc(Project(rightEye, glm::vec3(0, 0, 0))).x == Approx(-0.032f).margin(1e-5));
}

TEST_CASE("An asymmetric fov maps its own edges to the NDC edges", "[runtime][overlay]")
{
    const XrFovf fov = {-0.9f, 0.7f, 0.8f, -0.6f};
    const glm::mat4 m = OverlayClipFromLayer(Pose(0, 0, 0), fov, Pose(0, 0, 0), Pose(0, 0, 0));
    // A point on the left frustum edge at 1 m: x = tan(angleLeft).
    REQUIRE(Ndc(Project(m, glm::vec3(std::tan(-0.9f), 0, -1))).x == Approx(-1.0f).margin(1e-5));
    REQUIRE(Ndc(Project(m, glm::vec3(std::tan(0.7f), 0, -1))).x == Approx(1.0f).margin(1e-5));
    REQUIRE(Ndc(Project(m, glm::vec3(0, std::tan(0.8f) * 3, -3))).y == Approx(1.0f).margin(1e-5));
    REQUIRE(Ndc(Project(m, glm::vec3(0, std::tan(-0.6f) * 3, -3))).y == Approx(-1.0f).margin(1e-5));
}

TEST_CASE("Points behind the eye or inside the near plane are clipped", "[runtime][overlay]")
{
    const glm::mat4 m = OverlayClipFromLayer(Pose(0, 0, 0), SymmetricFov(kPi / 4), Pose(0, 0, 0), Pose(0, 0, 0));
    // Metal keeps 0 <= z <= w; behind the eye w < 0, inside the near plane z > w.
    const glm::vec4 behind = Project(m, glm::vec3(0, 0, 1));
    REQUIRE_FALSE((behind.z >= 0.0f && behind.z <= behind.w));
    const glm::vec4 tooClose = Project(m, glm::vec3(0, 0, -kOverlayNearClip / 2));
    REQUIRE_FALSE((tooClose.z >= 0.0f && tooClose.z <= tooClose.w));
    const glm::vec4 ahead = Project(m, glm::vec3(0, 0, -5));
    REQUIRE((ahead.z >= 0.0f && ahead.z <= ahead.w));
}

TEST_CASE("A cylinder puts its image centre ahead and its right edge to the right", "[runtime][overlay]")
{
    const float radius = 2.0f;
    const float angle = kPi / 2;
    REQUIRE(CylinderHeight(radius, angle, 2.0f) == Approx(radius * angle / 2.0f));

    const glm::vec3 centre = OverlayCylinderPoint(radius, angle, 1.0f, 0.5f, 0.5f);
    REQUIRE(centre.x == Approx(0.0f).margin(1e-6));
    REQUIRE(centre.z == Approx(-radius));

    // The right edge is 45 degrees to the right of straight ahead, still at the radius.
    const glm::vec3 rightTop = OverlayCylinderPoint(radius, angle, 1.0f, 1.0f, 0.0f);
    REQUIRE(std::atan2(rightTop.x, -rightTop.z) == Approx(kPi / 4));
    REQUIRE(std::hypot(rightTop.x, rightTop.z) == Approx(radius));
    REQUIRE(rightTop.y == Approx(0.5f));

    REQUIRE(IsDrawableCylinder(radius, angle, 1.0f));
}

TEST_CASE("An infinite cylinder is placed by orientations only", "[runtime][overlay]")
{
    REQUIRE(IsInfiniteCylinder(0.0f));
    REQUIRE(IsInfiniteCylinder(INFINITY));
    REQUIRE_FALSE(IsInfiniteCylinder(2.0f));
    REQUIRE(IsDrawableCylinder(0.0f, kPi / 4, 1.0f));
    REQUIRE(IsDrawableCylinder(INFINITY, kPi / 4, 1.0f));

    // Drawn at radius 1: the right edge is 22.5 degrees right, whatever the positions.
    const float height = CylinderHeight(1.0f, kPi / 4, 1.0f);
    const glm::vec3 centre = OverlayCylinderPoint(1.0f, kPi / 4, height, 0.5f, 0.5f);
    const glm::vec3 rightEdge = OverlayCylinderPoint(1.0f, kPi / 4, height, 1.0f, 0.5f);
    const glm::mat4 moved = OverlayClipFromLayerAtInfinity(Pose(0.032f, 0.1f, 0.5f), SymmetricFov(kPi / 4),
                                                           Pose(3, 1, 0), Pose(0, 0, -2));
    REQUIRE(Ndc(Project(moved, centre)).x == Approx(0.0f).margin(1e-5));
    REQUIRE(Ndc(Project(moved, centre)).y == Approx(0.0f).margin(1e-5));
    REQUIRE(Ndc(Project(moved, rightEdge)).x == Approx(std::tan(kPi / 8)).margin(1e-5));

    // Orientations still count: a layer turned 0.3 rad left sits tan(0.3) left of centre,
    // and an eye turned the same way looks straight at it again.
    const glm::mat4 turnedLayer = OverlayClipFromLayerAtInfinity(Pose(0.032f, 0, 0), SymmetricFov(kPi / 4),
                                                                 Pose(0, 0, 0), Pose(1, 0, -2, 0.3f));
    REQUIRE(Ndc(Project(turnedLayer, centre)).x == Approx(-std::tan(0.3f)).margin(1e-5));
    const glm::mat4 turnedBoth = OverlayClipFromLayerAtInfinity(Pose(0.032f, 0, 0, 0.3f), SymmetricFov(kPi / 4),
                                                                Pose(0, 0, 0), Pose(1, 0, -2, 0.3f));
    REQUIRE(Ndc(Project(turnedBoth, centre)).x == Approx(0.0f).margin(1e-5));
}
