// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "InputManager.h"
#include "TrackingReceiver.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <glm/gtc/quaternion.hpp>
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
    // The interaction profile stays bound (sticky) while the client is connected even
    // though the controller went idle — only activity drops, like real runtimes.
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) ==
          "/interaction_profiles/meta/touch_controller_quest_2");
    left = im.GetControllerPose(InputManager::Hand::Left);
    CHECK_THAT(left.position.x, WithinAbs(-0.35f, 0.001f));
    CHECK_THAT(left.position.y, WithinAbs(1.20f, 0.001f));
    CHECK_THAT(left.position.z, WithinAbs(-0.55f, 0.001f));
}

TEST_CASE("InputManager — controller profile is sticky per client while streaming", "[input]")
{
    InputManager im;
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);
    im.SetStreamingClientName("Meta Quest 3");

    // Only the RIGHT controller wakes: the profile resolves for BOTH hands at once so
    // the left controller waking later never changes the two-hand signature (a second
    // XrEventDataInteractionProfileChanged would make Unity churn its input devices).
    oxr::protocol::TrackingPacket rightOnly = {};
    rightOnly.timestampNs = 1'000'000'000;
    rightOnly.headOrientation[3] = 1.0f;
    rightOnly.trackingFlags = oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE;
    rightOnly.rightControllerRot[3] = 1.0f;
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&rightOnly), sizeof(rightOnly));
    im.Update(0.0f);

    CHECK(im.HasResolvedControllerProfile());
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) ==
          "/interaction_profiles/oculus/touch_controller");
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Right) ==
          "/interaction_profiles/oculus/touch_controller");
    CHECK_FALSE(im.IsControllerTrackingActive(InputManager::Hand::Left));
    CHECK(im.IsControllerTrackingActive(InputManager::Hand::Right));

    // Both controllers idle (system overlay / controllers asleep): profile stays bound.
    oxr::protocol::TrackingPacket idle = {};
    idle.timestampNs = 1'011'111'111;
    idle.headOrientation[3] = 1.0f;
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&idle), sizeof(idle));
    im.Update(0.0f);

    CHECK(im.HasResolvedControllerProfile());
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) ==
          "/interaction_profiles/oculus/touch_controller");
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Right) ==
          "/interaction_profiles/oculus/touch_controller");

    // Genuine disconnect: the sticky profile unbinds with the client.
    im.SetTrackingReceiver(nullptr);
    CHECK_FALSE(im.HasResolvedControllerProfile());
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left).empty());
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Right).empty());
}

TEST_CASE("InputManager — active hand tracking outranks the sticky controller profile", "[input]")
{
    InputManager im;
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);
    im.SetStreamingClientName("Meta Quest 3");

    oxr::protocol::TrackingPacket controllers = {};
    controllers.timestampNs = 1'000'000'000;
    controllers.headOrientation[3] = 1.0f;
    controllers.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE |
                                oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE;
    controllers.leftControllerRot[3] = 1.0f;
    controllers.rightControllerRot[3] = 1.0f;
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&controllers), sizeof(controllers));
    im.Update(0.0f);

    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) ==
          "/interaction_profiles/oculus/touch_controller");

    // Switch to hand tracking: live hands must win over the sticky controller profile.
    oxr::protocol::TrackingPacket hands = {};
    hands.timestampNs = 1'011'111'111;
    hands.headOrientation[3] = 1.0f;
    hands.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_HAND_ACTIVE |
                          oxr::protocol::TRACKING_FLAG_RIGHT_HAND_ACTIVE;
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&hands), sizeof(hands));
    im.Update(0.0f);

    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) ==
          "/interaction_profiles/ext/hand_interaction_ext");
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Right) ==
          "/interaction_profiles/ext/hand_interaction_ext");
    CHECK(im.HasResolvedControllerProfile());
}

TEST_CASE("InputManager — no fabricated profile without a client unless dev flag set", "[input]")
{
    InputManager im;

    // Default (no client, flag off): no interaction profile exists — real runtimes
    // report none until a controller is bound.
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left).empty());
    CHECK(im.GetCurrentInteractionProfileCandidates(InputManager::Hand::Left).empty());
    CHECK(im.GetCurrentInteractionProfileCandidates(InputManager::Hand::Right).empty());

    // Dev flag (simple_controller_fallback in config.toml): fabricate khr/simple for
    // client-less testing.
    im.SetSimpleControllerFallback(true);
    const auto candidates = im.GetCurrentInteractionProfileCandidates(InputManager::Hand::Left);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0] == "/interaction_profiles/khr/simple_controller");
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
        {"Meta Quest 3", "/interaction_profiles/oculus/touch_controller"},
        {"Quest 3", "/interaction_profiles/oculus/touch_controller"},
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

    constexpr const char* TouchProfile = "/interaction_profiles/oculus/touch_controller";
    constexpr const char* HandProfile = "/interaction_profiles/ext/hand_interaction_ext";

    CHECK(im.IsControllerTrackingActive(InputManager::Hand::Left));
    CHECK(im.IsHandTrackingActive(InputManager::Hand::Left));
    CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) == TouchProfile);

    std::vector<std::string> activeProfiles = im.GetActiveInteractionProfiles(InputManager::Hand::Left);
    CHECK(std::find(activeProfiles.begin(), activeProfiles.end(), TouchProfile) !=
          activeProfiles.end());
    CHECK(std::find(activeProfiles.begin(), activeProfiles.end(), HandProfile) !=
          activeProfiles.end());
    CHECK(std::find(activeProfiles.begin(), activeProfiles.end(),
                    "/interaction_profiles/khr/simple_controller") != activeProfiles.end());

    CHECK_THAT(im.GetFloatComponentForProfile(InputManager::Hand::Left,
                                              "trigger/value", TouchProfile),
               WithinAbs(0.20f, 0.001f));
    CHECK_THAT(im.GetFloatComponentForProfile(InputManager::Hand::Left,
                                              "squeeze/value", TouchProfile),
               WithinAbs(0.10f, 0.001f));
    XrVector2f stick = im.GetVector2fComponentForProfile(InputManager::Hand::Left,
                                                         "thumbstick", TouchProfile);
    CHECK_THAT(stick.x, WithinAbs(0.25f, 0.001f));
    CHECK_THAT(stick.y, WithinAbs(-0.50f, 0.001f));

    XrPosef controllerPose = im.GetPoseComponentForProfile(InputManager::Hand::Left,
                                                           "grip/pose", TouchProfile);
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

TEST_CASE("InputManager — streamed aim pose routes to aim/pose while grip is unchanged", "[input]")
{
    InputManager im;
    TrackingReceiver receiver;
    im.SetTrackingReceiver(&receiver);
    im.SetStreamingClientName("Oculus Quest");

    constexpr const char* TouchProfile = "/interaction_profiles/oculus/touch_controller";

    // Distinct grip and aim poses for BOTH hands, with left/right values that would not
    // survive a member swap (different signs and magnitudes per hand).
    const glm::quat leftAimRot = glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::quat rightAimRot = glm::angleAxis(-0.3f, glm::vec3(1.0f, 0.0f, 0.0f));

    oxr::protocol::TrackingPacket packet = {};
    packet.timestampNs = 1'000'000'000;
    packet.headOrientation[3] = 1.0f;
    packet.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE |
                           oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE;
    packet.leftControllerPos[0] = -0.40f;
    packet.leftControllerPos[1] = 1.10f;
    packet.leftControllerPos[2] = -0.60f;
    packet.leftControllerRot[3] = 1.0f;
    packet.rightControllerPos[0] = 0.45f;
    packet.rightControllerPos[1] = 1.15f;
    packet.rightControllerPos[2] = -0.55f;
    packet.rightControllerRot[3] = 1.0f;
    packet.leftAimPos[0] = -0.10f;
    packet.leftAimPos[1] = 1.50f;
    packet.leftAimPos[2] = -0.90f;
    packet.leftAimRot[0] = leftAimRot.x;
    packet.leftAimRot[1] = leftAimRot.y;
    packet.leftAimRot[2] = leftAimRot.z;
    packet.leftAimRot[3] = leftAimRot.w;
    packet.rightAimPos[0] = 0.20f;
    packet.rightAimPos[1] = 1.55f;
    packet.rightAimPos[2] = -0.85f;
    packet.rightAimRot[0] = rightAimRot.x;
    packet.rightAimRot[1] = rightAimRot.y;
    packet.rightAimRot[2] = rightAimRot.z;
    packet.rightAimRot[3] = rightAimRot.w;
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    im.Update(0.0f);

    SECTION("aim/pose returns the distinct streamed aim transform, grip/pose the grip pose")
    {
        XrPosef leftAim = im.GetPoseComponentForProfile(InputManager::Hand::Left,
                                                        "aim/pose", TouchProfile);
        CHECK_THAT(leftAim.position.x, WithinAbs(-0.10f, 0.001f));
        CHECK_THAT(leftAim.position.y, WithinAbs(1.50f, 0.001f));
        CHECK_THAT(leftAim.position.z, WithinAbs(-0.90f, 0.001f));
        CHECK_THAT(leftAim.orientation.y, WithinAbs(leftAimRot.y, 0.001f));
        CHECK_THAT(leftAim.orientation.w, WithinAbs(leftAimRot.w, 0.001f));

        XrPosef leftGrip = im.GetPoseComponentForProfile(InputManager::Hand::Left,
                                                         "grip/pose", TouchProfile);
        CHECK_THAT(leftGrip.position.x, WithinAbs(-0.40f, 0.001f));
        CHECK_THAT(leftGrip.position.y, WithinAbs(1.10f, 0.001f));
        CHECK_THAT(leftGrip.position.z, WithinAbs(-0.60f, 0.001f));
        CHECK_THAT(leftGrip.orientation.w, WithinAbs(1.0f, 0.001f));

        XrPosef rightAim = im.GetPoseComponentForProfile(InputManager::Hand::Right,
                                                         "aim/pose", TouchProfile);
        CHECK_THAT(rightAim.position.x, WithinAbs(0.20f, 0.001f));
        CHECK_THAT(rightAim.position.y, WithinAbs(1.55f, 0.001f));
        CHECK_THAT(rightAim.position.z, WithinAbs(-0.85f, 0.001f));
        CHECK_THAT(rightAim.orientation.x, WithinAbs(rightAimRot.x, 0.001f));
        CHECK_THAT(rightAim.orientation.w, WithinAbs(rightAimRot.w, 0.001f));

        XrPosef rightGrip = im.GetPoseComponentForProfile(InputManager::Hand::Right,
                                                          "grip/pose", TouchProfile);
        CHECK_THAT(rightGrip.position.x, WithinAbs(0.45f, 0.001f));
        CHECK_THAT(rightGrip.position.y, WithinAbs(1.15f, 0.001f));
        CHECK_THAT(rightGrip.position.z, WithinAbs(-0.55f, 0.001f));
    }

    SECTION("A zeroed aim quaternion falls the aim pose back to the grip pose")
    {
        oxr::protocol::TrackingPacket noAim = {};
        noAim.timestampNs = 1'011'111'111;
        noAim.headOrientation[3] = 1.0f;
        noAim.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE;
        noAim.leftControllerPos[0] = -0.42f;
        noAim.leftControllerPos[1] = 1.12f;
        noAim.leftControllerPos[2] = -0.62f;
        noAim.leftControllerRot[3] = 1.0f;
        // leftAimRot left zeroed → DecodeAimPose rejects it → aim/pose == grip/pose.
        receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&noAim), sizeof(noAim));
        im.Update(0.0f);

        XrPosef aim = im.GetPoseComponentForProfile(InputManager::Hand::Left,
                                                    "aim/pose", TouchProfile);
        XrPosef grip = im.GetPoseComponentForProfile(InputManager::Hand::Left,
                                                     "grip/pose", TouchProfile);
        CHECK_THAT(aim.position.x, WithinAbs(grip.position.x, 0.001f));
        CHECK_THAT(aim.position.y, WithinAbs(grip.position.y, 0.001f));
        CHECK_THAT(aim.position.z, WithinAbs(grip.position.z, 0.001f));
        CHECK_THAT(aim.orientation.w, WithinAbs(grip.orientation.w, 0.001f));
    }
}

TEST_CASE("TrackingReceiver — accepts pre-aim packets and rejects sub-minimum ones", "[input]")
{
    constexpr size_t kPreAimSize = offsetof(oxr::protocol::TrackingPacket, leftAimPos);

    oxr::protocol::TrackingPacket packet = {};
    packet.timestampNs = 1'000'000'000;
    packet.headPosition[0] = 0.5f;
    packet.headPosition[1] = 1.7f;
    packet.headPosition[2] = -0.3f;
    packet.headOrientation[3] = 1.0f;
    packet.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE;
    packet.leftControllerPos[0] = -0.33f;
    packet.leftControllerPos[1] = 1.05f;
    packet.leftControllerPos[2] = -0.52f;
    packet.leftControllerRot[3] = 1.0f;

    SECTION("A truncated pre-aim packet is accepted and aim falls back to grip")
    {
        InputManager im;
        TrackingReceiver receiver;
        im.SetTrackingReceiver(&receiver);
        im.SetStreamingClientName("Oculus Quest");
        // Inject exactly the pre-aim byte count — an older client that predates aim fields.
        receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), kPreAimSize);
        im.Update(0.0f);

        constexpr const char* TouchProfile = "/interaction_profiles/oculus/touch_controller";
        CHECK(im.IsControllerTrackingActive(InputManager::Hand::Left));
        XrPosef grip = im.GetPoseComponentForProfile(InputManager::Hand::Left,
                                                     "grip/pose", TouchProfile);
        CHECK_THAT(grip.position.x, WithinAbs(-0.33f, 0.001f));
        CHECK_THAT(grip.position.y, WithinAbs(1.05f, 0.001f));
        CHECK_THAT(grip.position.z, WithinAbs(-0.52f, 0.001f));

        // Absent aim fields stay zero, so aim/pose falls back to the grip pose.
        XrPosef aim = im.GetPoseComponentForProfile(InputManager::Hand::Left,
                                                    "aim/pose", TouchProfile);
        CHECK_THAT(aim.position.x, WithinAbs(grip.position.x, 0.001f));
        CHECK_THAT(aim.position.y, WithinAbs(grip.position.y, 0.001f));
        CHECK_THAT(aim.position.z, WithinAbs(grip.position.z, 0.001f));
    }

    SECTION("A packet below the minimum size is rejected, leaving state untouched")
    {
        TrackingReceiver receiver;
        receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), kPreAimSize - 1);

        oxr::protocol::TrackingPacket latest = {};
        CHECK_FALSE(receiver.GetLatestPose(latest));
    }
}

TEST_CASE("InputManager — profile re-resolves on reconnect and falls back without a client name",
          "[input]")
{
    SECTION("Reconnecting a different client re-resolves the profile, not the old sticky one")
    {
        InputManager im;
        TrackingReceiver first;
        im.SetTrackingReceiver(&first);
        im.SetStreamingClientName("Meta Quest 2");

        oxr::protocol::TrackingPacket packet = {};
        packet.timestampNs = 1'000'000'000;
        packet.headOrientation[3] = 1.0f;
        packet.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE;
        packet.leftControllerRot[3] = 1.0f;
        first.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
        im.Update(0.0f);
        CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) ==
              "/interaction_profiles/meta/touch_controller_quest_2");

        // Disconnect clears the sticky profile.
        im.SetTrackingReceiver(nullptr);
        CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left).empty());

        // A fresh client (PICO 4) must resolve to its own profile, not resurrect Quest 2.
        TrackingReceiver second;
        im.SetTrackingReceiver(&second);
        im.SetStreamingClientName("PICO 4");
        second.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
        im.Update(0.0f);
        CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) ==
              "/interaction_profiles/bytedance/pico4_controller");
    }

    SECTION("A controller waking before the client name resolves falls back to oculus/touch")
    {
        InputManager im;
        TrackingReceiver receiver;
        im.SetTrackingReceiver(&receiver);
        // No SetStreamingClientName — the empty-name fallback branch must engage.

        oxr::protocol::TrackingPacket packet = {};
        packet.timestampNs = 1'000'000'000;
        packet.headOrientation[3] = 1.0f;
        packet.trackingFlags = oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE;
        packet.leftControllerRot[3] = 1.0f;
        receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
        im.Update(0.0f);

        CHECK(im.GetCurrentInteractionProfile(InputManager::Hand::Left) ==
              "/interaction_profiles/oculus/touch_controller");
    }
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
