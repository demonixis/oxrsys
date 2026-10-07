// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "InputManager.h"
#include "TrackingReceiver.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <thread>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{

void SetLeftHandJoint(oxr::protocol::TrackingPacket& packet, uint32_t joint,
                      float x, float y, float z, float radius = 0.01f)
{
    packet.leftHandJoints[joint][0] = x;
    packet.leftHandJoints[joint][1] = y;
    packet.leftHandJoints[joint][2] = z;
    packet.leftHandJoints[joint][3] = radius;
}

void PopulateLeftPinchingHand(oxr::protocol::TrackingPacket& packet,
                              float palmX, float palmY, float palmZ)
{
    SetLeftHandJoint(packet, XR_HAND_JOINT_PALM_EXT, palmX, palmY, palmZ, 0.025f);
    SetLeftHandJoint(packet, XR_HAND_JOINT_WRIST_EXT, palmX, palmY - 0.08f, palmZ + 0.02f, 0.020f);
    SetLeftHandJoint(packet, XR_HAND_JOINT_INDEX_METACARPAL_EXT,
                     palmX - 0.03f, palmY + 0.01f, palmZ - 0.02f);
    SetLeftHandJoint(packet, XR_HAND_JOINT_LITTLE_METACARPAL_EXT,
                     palmX + 0.03f, palmY + 0.01f, palmZ - 0.02f);

    SetLeftHandJoint(packet, XR_HAND_JOINT_THUMB_TIP_EXT,
                     palmX - 0.008f, palmY + 0.01f, palmZ - 0.04f);
    SetLeftHandJoint(packet, XR_HAND_JOINT_INDEX_TIP_EXT,
                     palmX + 0.007f, palmY + 0.01f, palmZ - 0.04f);
    SetLeftHandJoint(packet, XR_HAND_JOINT_MIDDLE_TIP_EXT,
                     palmX, palmY + 0.01f, palmZ - 0.034f);
    SetLeftHandJoint(packet, XR_HAND_JOINT_RING_TIP_EXT,
                     palmX + 0.010f, palmY + 0.01f, palmZ - 0.034f);
    SetLeftHandJoint(packet, XR_HAND_JOINT_LITTLE_TIP_EXT,
                     palmX + 0.020f, palmY + 0.01f, palmZ - 0.034f);
    SetLeftHandJoint(packet, XR_HAND_JOINT_INDEX_PROXIMAL_EXT,
                     palmX - 0.015f, palmY + 0.01f, palmZ - 0.025f);
}

} // namespace

TEST_CASE("InputManager — initial state", "[input]")
{
    InputManager im;

    SECTION("Head starts at default position")
    {
        XrPosef pose = im.GetHeadPose();
        CHECK_THAT(pose.position.x, WithinAbs(0.0, 0.001));
        CHECK_THAT(pose.position.y, WithinAbs(1.6, 0.001));
        CHECK_THAT(pose.position.z, WithinAbs(0.0, 0.001));
        // Identity orientation (no rotation)
        CHECK_THAT(pose.orientation.w, WithinAbs(1.0, 0.001));
    }

    SECTION("Controllers at default positions")
    {
        XrPosef left = im.GetControllerPose(InputManager::Hand::Left);
        CHECK_THAT(left.position.x, WithinAbs(-0.2, 0.001));
        CHECK_THAT(left.position.y, WithinAbs(1.3, 0.001));
        CHECK_THAT(left.position.z, WithinAbs(-0.4, 0.001));

        XrPosef right = im.GetControllerPose(InputManager::Hand::Right);
        CHECK_THAT(right.position.x, WithinAbs(0.2, 0.001));
        CHECK_THAT(right.position.y, WithinAbs(1.3, 0.001));
        CHECK_THAT(right.position.z, WithinAbs(-0.4, 0.001));
    }

    SECTION("Mode defaults to Controller")
    {
        CHECK(im.GetInputMode() == InputManager::InputMode::Controller);
    }

    SECTION("Buttons default to released")
    {
        CHECK(im.GetGrabValue(InputManager::Hand::Left) == 0.0f);
        CHECK(im.GetGrabValue(InputManager::Hand::Right) == 0.0f);
        CHECK(im.GetMenuClick() == false);
    }

    SECTION("Not streaming by default")
    {
        CHECK(im.IsStreaming() == false);
    }
}

TEST_CASE("InputManager — hand joint generation", "[input]")
{
    InputManager im;

    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT] = {};
    im.GetHandJointLocations(InputManager::Hand::Left, joints, XR_HAND_JOINT_COUNT_EXT);

    SECTION("All 26 joints have valid flags")
    {
        for (uint32_t i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++)
        {
            CHECK((joints[i].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0);
            CHECK((joints[i].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0);
        }
    }

    SECTION("All joints have positive radius")
    {
        for (uint32_t i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++)
        {
            CHECK(joints[i].radius > 0.0f);
        }
    }

    SECTION("Palm is at controller position")
    {
        XrPosef ctrl = im.GetControllerPose(InputManager::Hand::Left);
        CHECK_THAT(joints[0].pose.position.x, WithinAbs(ctrl.position.x, 0.001));
        CHECK_THAT(joints[0].pose.position.y, WithinAbs(ctrl.position.y, 0.001));
        CHECK_THAT(joints[0].pose.position.z, WithinAbs(ctrl.position.z, 0.001));
    }
}

TEST_CASE("InputManager exposes only explicitly valid finite head velocities", "[input][velocity]")
{
    InputManager im;
    CHECK(im.GetHeadVelocity().velocityFlags == 0);
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);
    CHECK(im.GetHeadVelocity().velocityFlags == 0);

    oxr::protocol::TrackingPacket packet = {};
    packet.headOrientation[3] = 1.0f;
    packet.leftControllerRot[3] = 1.0f;
    packet.rightControllerRot[3] = 1.0f;
    packet.trackingFlags = oxr::protocol::TRACKING_FLAG_HEAD_LINEAR_VELOCITY_VALID |
                           oxr::protocol::TRACKING_FLAG_HEAD_ANGULAR_VELOCITY_VALID;
    XrSpaceVelocityFlags expectedFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;

    SECTION("Explicitly measured stationary motion is valid")
    {
        packet.headLinearVelocity[0] = 0.0f;
        packet.headAngularVelocity[1] = 0.0f;
    }
    SECTION("Measured nonzero motion is preserved")
    {
        packet.headLinearVelocity[0] = 1.25f;
        packet.headAngularVelocity[1] = -2.5f;
    }
    SECTION("Legacy nonzero values without validity flags remain unavailable")
    {
        packet.trackingFlags = 0;
        packet.headLinearVelocity[0] = 2.0f;
        packet.headAngularVelocity[1] = 3.0f;
        expectedFlags = 0;
    }
    SECTION("Non-finite linear motion leaves valid angular motion intact")
    {
        packet.headLinearVelocity[0] = std::numeric_limits<float>::quiet_NaN();
        packet.headAngularVelocity[1] = 3.0f;
        expectedFlags = XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
    }
    SECTION("Non-finite angular motion leaves valid linear motion intact")
    {
        packet.headLinearVelocity[0] = 2.0f;
        packet.headAngularVelocity[1] = std::numeric_limits<float>::infinity();
        expectedFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
    }

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    const XrSpaceVelocity velocity = im.GetHeadVelocity();
    CHECK(velocity.velocityFlags == expectedFlags);
    if ((expectedFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0)
    {
        CHECK(velocity.linearVelocity.x == packet.headLinearVelocity[0]);
    }
    if ((expectedFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0)
    {
        CHECK(velocity.angularVelocity.y == packet.headAngularVelocity[1]);
    }
}

TEST_CASE("InputManager keeps grip and aim velocity sources distinct", "[input][velocity]")
{
    InputManager im;
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);
    oxr::protocol::TrackingPacket packet = {};
    packet.headOrientation[3] = 1.0f;
    packet.leftControllerRot[3] = 1.0f;
    packet.rightControllerRot[3] = 1.0f;
    packet.leftControllerAimRot[3] = 1.0f;
    packet.rightControllerAimRot[3] = 1.0f;
    packet.leftControllerAimPos[0] = -0.25f;
    packet.rightControllerAimPos[0] = 0.25f;
    packet.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE |
        oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE |
        oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_LINEAR_VELOCITY_VALID |
        oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ANGULAR_VELOCITY_VALID |
        oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_LINEAR_VELOCITY_VALID |
        oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ANGULAR_VELOCITY_VALID |
        oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_AIM_LINEAR_VELOCITY_VALID |
        oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_AIM_ANGULAR_VELOCITY_VALID |
        oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_AIM_LINEAR_VELOCITY_VALID |
        oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_AIM_ANGULAR_VELOCITY_VALID;
    packet.leftControllerLinearVelocity[0] = 1.0f;
    packet.leftControllerAngularVelocity[1] = 2.0f;
    packet.rightControllerLinearVelocity[0] = 3.0f;
    packet.rightControllerAngularVelocity[1] = 4.0f;
    packet.leftControllerAimLinearVelocity[0] = 5.0f;
    packet.leftControllerAimAngularVelocity[1] = 6.0f;
    packet.rightControllerAimLinearVelocity[0] = 7.0f;
    packet.rightControllerAimAngularVelocity[1] = 8.0f;

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    im.Update(0.0f);
    const std::string profile = "/interaction_profiles/oculus/touch_controller";
    const XrSpaceVelocity leftGrip = im.GetPoseVelocityForProfile(InputManager::Hand::Left, "grip/pose", profile);
    const XrSpaceVelocity rightGrip = im.GetPoseVelocityForProfile(InputManager::Hand::Right, "grip/pose", profile);
    const XrSpaceVelocity leftAim = im.GetPoseVelocityForProfile(InputManager::Hand::Left, "aim/pose", profile);
    const XrSpaceVelocity rightAim = im.GetPoseVelocityForProfile(InputManager::Hand::Right, "aim/pose", profile);
    const XrSpaceVelocityFlags validFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
    CHECK(leftGrip.velocityFlags == validFlags);
    CHECK(rightGrip.velocityFlags == validFlags);
    CHECK(leftAim.velocityFlags == validFlags);
    CHECK(rightAim.velocityFlags == validFlags);
    CHECK(leftGrip.linearVelocity.x == 1.0f);
    CHECK(leftGrip.angularVelocity.y == 2.0f);
    CHECK(rightGrip.linearVelocity.x == 3.0f);
    CHECK(rightGrip.angularVelocity.y == 4.0f);
    CHECK(leftAim.linearVelocity.x == 5.0f);
    CHECK(leftAim.angularVelocity.y == 6.0f);
    CHECK(rightAim.linearVelocity.x == 7.0f);
    CHECK(rightAim.angularVelocity.y == 8.0f);
}

TEST_CASE("InputManager does not attach controller motion to another pose source", "[input][velocity]")
{
    InputManager im;
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);
    oxr::protocol::TrackingPacket packet = {};
    packet.headOrientation[3] = 1.0f;
    packet.leftControllerRot[3] = 1.0f;
    packet.rightControllerRot[3] = 1.0f;
    packet.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE |
        oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_LINEAR_VELOCITY_VALID |
        oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ANGULAR_VELOCITY_VALID |
        oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_LINEAR_VELOCITY_VALID;
    packet.leftControllerLinearVelocity[0] = 2.0f;
    packet.rightControllerLinearVelocity[0] = 3.0f;
    std::string profile = "/interaction_profiles/oculus/touch_controller";
    bool expectedAvailable = false;

    SECTION("Inactive controller data is unavailable")
    {
        packet.trackingFlags &= ~oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE;
    }
    SECTION("Automation poses have no measured velocity")
    {
        XrPosef pose = {};
        pose.orientation.w = 1.0f;
        im.SetAutomationPose(InputManager::Hand::Left, "grip/pose", pose);
    }
    SECTION("Hand interaction never borrows controller velocity")
    {
        profile = "/interaction_profiles/ext/hand_interaction_ext";
    }
    SECTION("Unqualified hand poses never borrow controller velocity")
    {
        packet.trackingFlags |= oxr::protocol::TRACKING_FLAG_LEFT_HAND_ACTIVE;
        profile.clear();
    }
    SECTION("Measured zero angular velocity remains valid")
    {
        expectedAvailable = true;
    }

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    im.Update(0.0f);
    const XrSpaceVelocity velocity = im.GetPoseVelocityForProfile(InputManager::Hand::Left, "grip/pose", profile);
    CHECK(velocity.velocityFlags == (expectedAvailable
        ? XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT : 0));
    CHECK(im.GetPoseVelocityForProfile(InputManager::Hand::Right, "grip/pose", profile).velocityFlags == 0);
    CHECK(im.GetPoseVelocityForProfile(InputManager::Hand::Left, "pinch_ext/pose", profile).velocityFlags == 0);
}

TEST_CASE("InputManager aim velocity follows the legacy grip-pose fallback", "[input][velocity]")
{
    InputManager im;
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);
    oxr::protocol::TrackingPacket packet = {};
    packet.headOrientation[3] = 1.0f;
    packet.leftControllerRot[3] = 1.0f;
    packet.rightControllerRot[3] = 1.0f;
    packet.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE |
        oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_LINEAR_VELOCITY_VALID |
        oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_AIM_LINEAR_VELOCITY_VALID;
    packet.leftControllerLinearVelocity[0] = 1.0f;
    packet.leftControllerAimLinearVelocity[0] = 2.0f;
    for (float orientationW : {0.0f, 1.0f})
    {
        packet.leftControllerAimRot[3] = orientationW;
        receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
        im.Update(0.0f);
        const XrSpaceVelocity velocity = im.GetPoseVelocityForProfile(
            InputManager::Hand::Left, "aim/pose", "/interaction_profiles/oculus/touch_controller");
        CHECK(velocity.velocityFlags == XR_SPACE_VELOCITY_LINEAR_VALID_BIT);
        CHECK(velocity.linearVelocity.x == 1.0f);
    }
}

TEST_CASE("InputManager accepts only valid measured stage bounds", "[input][bounds]")
{
    InputManager im;
    XrExtent2Df bounds = {9.0f, 9.0f};
    CHECK_FALSE(im.GetStageBounds(bounds));
    CHECK(bounds.width == 0.0f);
    CHECK(bounds.height == 0.0f);
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);
    oxr::protocol::TrackingPacket packet = {};
    packet.headOrientation[3] = 1.0f;
    packet.leftControllerRot[3] = 1.0f;
    packet.rightControllerRot[3] = 1.0f;
    packet.trackingFlags = oxr::protocol::TRACKING_FLAG_STAGE_BOUNDS_VALID;
    packet.stageBoundsWidth = 2.25f;
    packet.stageBoundsHeight = 3.5f;
    bool expectedAvailable = true;

    SECTION("Measured rectangular bounds are preserved")
    {
        packet.stageBoundsWidth = 2.25f;
    }
    SECTION("A missing validity flag clears retained bounds")
    {
        packet.trackingFlags = 0;
        expectedAvailable = false;
    }
    SECTION("Zero dimensions are invalid")
    {
        packet.stageBoundsHeight = 0.0f;
        expectedAvailable = false;
    }
    SECTION("Negative dimensions are invalid")
    {
        packet.stageBoundsWidth = -2.0f;
        expectedAvailable = false;
    }
    SECTION("Non-finite dimensions are invalid")
    {
        packet.stageBoundsWidth = std::numeric_limits<float>::infinity();
        expectedAvailable = false;
    }

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    CHECK(im.GetStageBounds(bounds) == expectedAvailable);
    CHECK(bounds.width == (expectedAvailable ? packet.stageBoundsWidth : 0.0f));
    CHECK(bounds.height == (expectedAvailable ? packet.stageBoundsHeight : 0.0f));
}

TEST_CASE("InputManager expires measured velocities and stage bounds", "[input][velocity][bounds]")
{
    InputManager im;
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);
    oxr::protocol::TrackingPacket packet = {};
    packet.headOrientation[3] = 1.0f;
    packet.leftControllerRot[3] = 1.0f;
    packet.rightControllerRot[3] = 1.0f;
    packet.trackingFlags = oxr::protocol::TRACKING_FLAG_HEAD_LINEAR_VELOCITY_VALID |
        oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE |
        oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_LINEAR_VELOCITY_VALID |
        oxr::protocol::TRACKING_FLAG_STAGE_BOUNDS_VALID;
    packet.headLinearVelocity[0] = 1.0f;
    packet.leftControllerLinearVelocity[0] = 2.0f;
    packet.stageBoundsWidth = 2.0f;
    packet.stageBoundsHeight = 3.0f;
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    im.Update(0.0f);
    XrExtent2Df bounds = {};
    REQUIRE(im.GetHeadVelocity().velocityFlags == XR_SPACE_VELOCITY_LINEAR_VALID_BIT);
    REQUIRE(im.GetStageBounds(bounds));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(im.GetHeadVelocity().velocityFlags == 0);
    CHECK(im.GetPoseVelocityForProfile(InputManager::Hand::Left, "grip/pose", "").velocityFlags == 0);
    CHECK_FALSE(im.GetStageBounds(bounds));
    CHECK(bounds.width == 0.0f);
    CHECK(bounds.height == 0.0f);
}

TEST_CASE("InputManager — eye views", "[input]")
{
    InputManager im;

    XrView views[2] = {};
    im.GetEyeViews(views, 2);

    SECTION("Two views with valid types")
    {
        CHECK(views[0].type == XR_TYPE_VIEW);
        CHECK(views[1].type == XR_TYPE_VIEW);
    }

    SECTION("Left eye is to the left of right eye")
    {
        CHECK(views[0].pose.position.x < views[1].pose.position.x);
    }

    SECTION("FOV is set")
    {
        CHECK(views[0].fov.angleLeft < 0.0f);
        CHECK(views[0].fov.angleRight > 0.0f);
        CHECK(views[0].fov.angleUp > 0.0f);
        CHECK(views[0].fov.angleDown < 0.0f);
    }
}

TEST_CASE("InputManager — streaming eye data", "[input]")
{
    InputManager im;
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);

    oxr::protocol::TrackingPacket packet = {};
    packet.ipd = 0.070f;
    packet.eyeFov[0] = -1.10f;
    packet.eyeFov[1] = 0.90f;
    packet.eyeFov[2] = 1.00f;
    packet.eyeFov[3] = -0.95f;
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));

    im.Update(0.0f);

    XrView views[2] = {};
    im.GetEyeViews(views, 2);

    SECTION("Streaming IPD overrides the default eye separation")
    {
        CHECK_THAT(views[1].pose.position.x - views[0].pose.position.x, WithinAbs(0.070f, 0.001f));
    }

    SECTION("Streaming FOV is used for the left eye and mirrored for the right eye")
    {
        CHECK_THAT(views[0].fov.angleLeft, WithinAbs(-1.10f, 0.001f));
        CHECK_THAT(views[0].fov.angleRight, WithinAbs(0.90f, 0.001f));
        CHECK_THAT(views[0].fov.angleUp, WithinAbs(1.00f, 0.001f));
        CHECK_THAT(views[0].fov.angleDown, WithinAbs(-0.95f, 0.001f));

        CHECK_THAT(views[1].fov.angleLeft, WithinAbs(-0.90f, 0.001f));
        CHECK_THAT(views[1].fov.angleRight, WithinAbs(1.10f, 0.001f));
        CHECK_THAT(views[1].fov.angleUp, WithinAbs(1.00f, 0.001f));
        CHECK_THAT(views[1].fov.angleDown, WithinAbs(-0.95f, 0.001f));
    }
}

TEST_CASE("InputManager — streaming head pose", "[input]")
{
    InputManager im;
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);

    oxr::protocol::TrackingPacket packet = {};
    packet.headPosition[0] = 1.0f;
    packet.headPosition[1] = 2.0f;
    packet.headPosition[2] = 3.0f;
    glm::quat yaw45 = glm::angleAxis(0.785f, glm::vec3(0.0f, 1.0f, 0.0f));
    packet.headOrientation[0] = yaw45.x;
    packet.headOrientation[1] = yaw45.y;
    packet.headOrientation[2] = yaw45.z;
    packet.headOrientation[3] = yaw45.w;
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));

    im.Update(0.0f);

    XrPosef pose = im.GetHeadPose();
    CHECK_THAT(pose.position.x, WithinAbs(1.0, 0.001));
    CHECK_THAT(pose.position.y, WithinAbs(2.0, 0.001));
    CHECK_THAT(pose.position.z, WithinAbs(3.0, 0.001));
    CHECK(std::abs(pose.orientation.y) > 0.01f);
}

TEST_CASE("InputManager — streaming controller activity gates pose updates", "[input]")
{
    InputManager im;
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);
    im.SetStreamingClientName("Meta Quest 2");

    oxr::protocol::TrackingPacket active = {};
    active.timestampNs = 1'000'000'000;
    active.headOrientation[3] = 1.0f;
    active.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE |
                           oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE;
    active.leftControllerPos[0] = -0.35f;
    active.leftControllerPos[1] = 1.20f;
    active.leftControllerPos[2] = -0.55f;
    active.leftControllerRot[3] = 1.0f;
    active.rightControllerPos[0] = 0.35f;
    active.rightControllerPos[1] = 1.25f;
    active.rightControllerPos[2] = -0.50f;
    active.rightControllerRot[3] = 1.0f;

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&active), sizeof(active));
    im.Update(0.0f);

    CHECK(im.IsControllerTrackingActive(InputManager::Hand::Left));
    CHECK(im.IsInputDeviceActive(InputManager::Hand::Left));
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) ==
          "/interaction_profiles/meta/touch_controller_quest_2");
    XrPosef left = im.GetControllerPose(InputManager::Hand::Left);
    CHECK_THAT(left.position.x, WithinAbs(-0.35f, 0.001f));
    CHECK_THAT(left.position.y, WithinAbs(1.20f, 0.001f));
    CHECK_THAT(left.position.z, WithinAbs(-0.55f, 0.001f));

    oxr::protocol::TrackingPacket inactive = {};
    inactive.timestampNs = 1'011'111'111;
    inactive.headOrientation[3] = 1.0f;
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&inactive), sizeof(inactive));
    im.Update(0.0f);

    CHECK_FALSE(im.IsControllerTrackingActive(InputManager::Hand::Left));
    CHECK_FALSE(im.IsInputDeviceActive(InputManager::Hand::Left));
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left).empty());
    left = im.GetControllerPose(InputManager::Hand::Left);
    CHECK_THAT(left.position.x, WithinAbs(-0.35f, 0.001f));
    CHECK_THAT(left.position.y, WithinAbs(1.20f, 0.001f));
    CHECK_THAT(left.position.z, WithinAbs(-0.55f, 0.001f));
}

TEST_CASE("InputManager — streaming client names map to controller profiles and aliases", "[input]")
{
    struct Case
    {
        const char* clientName;
        const char* expectedProfile;
    };

    const Case cases[] = {
        {"Oculus Quest", "/interaction_profiles/oculus/touch_controller"},
        {"Meta Quest 1", "/interaction_profiles/meta/touch_controller_quest_1_rift_s"},
        {"Meta Quest 2", "/interaction_profiles/meta/touch_controller_quest_2"},
        {"Meta Quest 3", "/interaction_profiles/meta/touch_plus_controller"},
        {"Quest 3", "/interaction_profiles/meta/touch_plus_controller"},
        {"Unknown headset", "/interaction_profiles/oculus/touch_controller"},
        {"PICO Neo3", "/interaction_profiles/bytedance/pico_neo3_controller"},
        {"PICO 4", "/interaction_profiles/bytedance/pico4_controller"},
    };

    for (const Case& testCase : cases)
    {
        INFO(testCase.clientName);
        InputManager im;
        TrackingReceiver receiver;
        im.SetTrackingReceiver(&receiver);
        im.SetStreamingClientName(testCase.clientName);

        oxr::protocol::TrackingPacket packet = {};
        packet.timestampNs = 1'000'000'000;
        packet.headOrientation[3] = 1.0f;
        packet.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE;
        packet.leftControllerRot[3] = 1.0f;
        receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
        im.Update(0.0f);

        CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) == testCase.expectedProfile);
        std::vector<std::string> profiles = im.GetActiveInteractionProfiles(InputManager::Hand::Left);
        CHECK(std::find(profiles.begin(), profiles.end(), testCase.expectedProfile) != profiles.end());
        CHECK(std::find(profiles.begin(), profiles.end(),
                        "/interaction_profiles/khr/simple_controller") != profiles.end());
        if (std::string(testCase.expectedProfile).find("/interaction_profiles/meta/") == 0)
        {
            CHECK(std::find(profiles.begin(), profiles.end(),
                            "/interaction_profiles/oculus/touch_controller") != profiles.end());
        }
    }
}

TEST_CASE("InputManager — streaming hands and controllers stay profile separated", "[input]")
{
    InputManager im;
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);
    im.SetStreamingClientName("Meta Quest 3");

    oxr::protocol::TrackingPacket packet = {};
    packet.timestampNs = 1'000'000'000;
    packet.headOrientation[3] = 1.0f;
    packet.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE |
                           oxr::protocol::TRACKING_FLAG_LEFT_HAND_ACTIVE;
    packet.leftControllerPos[0] = -0.40f;
    packet.leftControllerPos[1] = 1.10f;
    packet.leftControllerPos[2] = -0.60f;
    packet.leftControllerRot[3] = 1.0f;
    packet.leftTrigger = 0.20f;
    packet.leftGrip = 0.10f;
    packet.leftThumbstick[0] = 0.25f;
    packet.leftThumbstick[1] = -0.50f;
    PopulateLeftPinchingHand(packet, 0.10f, 1.30f, -0.20f);

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    im.Update(0.0f);

    constexpr const char* TouchPlusProfile = "/interaction_profiles/meta/touch_plus_controller";
    constexpr const char* HandProfile = "/interaction_profiles/ext/hand_interaction_ext";

    CHECK(im.IsControllerTrackingActive(InputManager::Hand::Left));
    CHECK(im.IsHandTrackingActive(InputManager::Hand::Left));
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) == TouchPlusProfile);

    std::vector<std::string> activeProfiles = im.GetActiveInteractionProfiles(InputManager::Hand::Left);
    CHECK(std::find(activeProfiles.begin(), activeProfiles.end(), TouchPlusProfile) !=
          activeProfiles.end());
    CHECK(std::find(activeProfiles.begin(), activeProfiles.end(), HandProfile) !=
          activeProfiles.end());
    CHECK(std::find(activeProfiles.begin(), activeProfiles.end(),
                    "/interaction_profiles/khr/simple_controller") != activeProfiles.end());

    CHECK_THAT(im.GetFloatComponentForProfile(InputManager::Hand::Left,
                                              "trigger/value", TouchPlusProfile),
               WithinAbs(0.20f, 0.001f));
    CHECK_THAT(im.GetFloatComponentForProfile(InputManager::Hand::Left,
                                              "squeeze/value", TouchPlusProfile),
               WithinAbs(0.10f, 0.001f));
    XrVector2f stick = im.GetVector2fComponentForProfile(InputManager::Hand::Left,
                                                         "thumbstick", TouchPlusProfile);
    CHECK_THAT(stick.x, WithinAbs(0.25f, 0.001f));
    CHECK_THAT(stick.y, WithinAbs(-0.50f, 0.001f));

    XrPosef controllerPose = im.GetPoseComponentForProfile(InputManager::Hand::Left,
                                                           "grip/pose", TouchPlusProfile);
    CHECK_THAT(controllerPose.position.x, WithinAbs(-0.40f, 0.001f));
    CHECK_THAT(controllerPose.position.y, WithinAbs(1.10f, 0.001f));
    CHECK_THAT(controllerPose.position.z, WithinAbs(-0.60f, 0.001f));

    CHECK(im.GetFloatComponentForProfile(InputManager::Hand::Left,
                                         "pinch_ext/value", HandProfile) > 0.95f);
    CHECK(im.GetFloatComponentForProfile(InputManager::Hand::Left,
                                         "grasp_ext/value", HandProfile) > 0.75f);
    XrPosef handPose = im.GetPoseComponentForProfile(InputManager::Hand::Left,
                                                     "grip/pose", HandProfile);
    CHECK_THAT(handPose.position.x, WithinAbs(0.10f, 0.001f));
    CHECK_THAT(handPose.position.y, WithinAbs(1.30f, 0.001f));
    CHECK_THAT(handPose.position.z, WithinAbs(-0.20f, 0.001f));

    oxr::protocol::TrackingPacket handOnly = {};
    handOnly.timestampNs = 1'011'111'111;
    handOnly.headOrientation[3] = 1.0f;
    handOnly.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_HAND_ACTIVE;
    PopulateLeftPinchingHand(handOnly, 0.20f, 1.35f, -0.25f);
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&handOnly), sizeof(handOnly));
    im.Update(0.0f);

    CHECK_FALSE(im.IsControllerTrackingActive(InputManager::Hand::Left));
    CHECK(im.IsHandTrackingActive(InputManager::Hand::Left));
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) == HandProfile);

    XrPosef lastControllerPose = im.GetControllerPose(InputManager::Hand::Left);
    CHECK_THAT(lastControllerPose.position.x, WithinAbs(-0.40f, 0.001f));
    CHECK_THAT(lastControllerPose.position.y, WithinAbs(1.10f, 0.001f));
    CHECK_THAT(lastControllerPose.position.z, WithinAbs(-0.60f, 0.001f));

    handPose = im.GetPoseComponentForProfile(InputManager::Hand::Left,
                                             "grip/pose", HandProfile);
    CHECK_THAT(handPose.position.x, WithinAbs(0.20f, 0.001f));
    CHECK_THAT(handPose.position.y, WithinAbs(1.35f, 0.001f));
    CHECK_THAT(handPose.position.z, WithinAbs(-0.25f, 0.001f));
}

TEST_CASE("InputManager — select follows trigger and squeeze follows grab", "[input]")
{
    InputManager im;
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);
    im.SetStreamingClientName("Oculus Quest");

    oxr::protocol::TrackingPacket packet = {};
    packet.timestampNs = 1'000'000'000;
    packet.headOrientation[3] = 1.0f;
    packet.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE;
    packet.leftControllerRot[3] = 1.0f;
    packet.leftTrigger = 0.75f;
    packet.leftGrip = 0.20f;
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    im.Update(0.0f);

    CHECK_THAT(im.GetFloatComponent(InputManager::Hand::Left, "select/value"),
               WithinAbs(0.75f, 0.001f));
    CHECK_THAT(im.GetFloatComponent(InputManager::Hand::Left, "trigger/value"),
               WithinAbs(0.75f, 0.001f));
    CHECK_THAT(im.GetFloatComponent(InputManager::Hand::Left, "squeeze/value"),
               WithinAbs(0.20f, 0.001f));
    CHECK(im.GetButtonClick(InputManager::Hand::Left, "select/click"));
    CHECK_FALSE(im.GetButtonClick(InputManager::Hand::Left, "squeeze/click"));
}

TEST_CASE("TrackingReceiver — predicted pose extrapolates recent motion", "[input]")
{
    TrackingReceiver receiver;

    oxr::protocol::TrackingPacket first = {};
    first.timestampNs = 1'000'000'000;
    first.headPosition[0] = 0.000f;
    first.headOrientation[3] = 1.0f;

    oxr::protocol::TrackingPacket second = {};
    second.timestampNs = 1'011'111'111;
    second.headPosition[0] = 0.020f;
    glm::quat yaw10 = glm::angleAxis(0.1745f, glm::vec3(0.0f, 1.0f, 0.0f));
    second.headOrientation[0] = yaw10.x;
    second.headOrientation[1] = yaw10.y;
    second.headOrientation[2] = yaw10.z;
    second.headOrientation[3] = yaw10.w;

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&first), sizeof(first));
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&second), sizeof(second));
    receiver.SetPredictionHorizonMs(11.0f);

    oxr::protocol::TrackingPacket predicted = {};
    REQUIRE(receiver.GetPredictedPose(predicted));

    SECTION("Head position advances beyond the latest packet")
    {
        CHECK(predicted.headPosition[0] > second.headPosition[0]);
    }

    SECTION("Head orientation advances beyond the latest packet")
    {
        CHECK(std::abs(predicted.headOrientation[1]) > std::abs(second.headOrientation[1]));
    }
}

TEST_CASE("TrackingReceiver — controller prediction requires active history", "[input]")
{
    TrackingReceiver receiver;

    oxr::protocol::TrackingPacket inactive = {};
    inactive.timestampNs = 1'000'000'000;
    inactive.headOrientation[3] = 1.0f;

    oxr::protocol::TrackingPacket active = {};
    active.timestampNs = 1'011'111'111;
    active.headOrientation[3] = 1.0f;
    active.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE;
    active.leftControllerPos[0] = -0.30f;
    active.leftControllerPos[1] = 1.10f;
    active.leftControllerPos[2] = -0.50f;
    active.leftControllerRot[3] = 1.0f;

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&inactive), sizeof(inactive));
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&active), sizeof(active));
    receiver.SetPredictionHorizonMs(20.0f);

    oxr::protocol::TrackingPacket predicted = {};
    REQUIRE(receiver.GetPredictedPose(predicted));

    CHECK((predicted.trackingFlags & oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE) != 0);
    CHECK_THAT(predicted.leftControllerPos[0], WithinAbs(active.leftControllerPos[0], 0.001f));
    CHECK_THAT(predicted.leftControllerPos[1], WithinAbs(active.leftControllerPos[1], 0.001f));
    CHECK_THAT(predicted.leftControllerPos[2], WithinAbs(active.leftControllerPos[2], 0.001f));
}

TEST_CASE("TrackingReceiver — angular velocity uses the full prediction horizon", "[input]")
{
    TrackingReceiver receiver;

    oxr::protocol::TrackingPacket first = {};
    first.timestampNs = 2'000'000'000;
    first.headOrientation[3] = 1.0f;

    oxr::protocol::TrackingPacket second = {};
    second.timestampNs = 2'011'111'111;
    second.headOrientation[3] = 1.0f;
    second.headAngularVelocity[1] = 1.0f;

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&first), sizeof(first));
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&second), sizeof(second));
    receiver.SetPredictionHorizonMs(20.0f);

    oxr::protocol::TrackingPacket predicted = {};
    REQUIRE(receiver.GetPredictedPose(predicted));

    CHECK_THAT(predicted.headOrientation[1], WithinAbs(std::sin(0.010f), 0.001f));
    CHECK_THAT(predicted.headOrientation[3], WithinAbs(std::cos(0.010f), 0.001f));
}
