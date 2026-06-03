// SPDX-License-Identifier: MPL-2.0

#include "InputManager.h"
#include "Config.h"
#include "Instance.h"
#include "TrackingReceiver.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <spdlog/spdlog.h>

// XR_EXT_hand_tracking types are in openxr.h

namespace
{

size_t HandIndex(InputManager::Hand hand)
{
    return (hand == InputManager::Hand::Left) ? 0u : 1u;
}

std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ContainsAny(const std::string& value, std::initializer_list<const char*> needles)
{
    for (const char* needle : needles)
    {
        if (value.find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

std::string DetectStreamingControllerProfile(const std::string& clientName)
{
    const std::string lowerName = Lowercase(clientName);

    if (ContainsAny(lowerName, {"pico 3", "pico3", "neo 3", "neo3"}))
    {
        return "/interaction_profiles/bytedance/pico_neo3_controller";
    }
    if (ContainsAny(lowerName, {"pico 4", "pico4"}))
    {
        return "/interaction_profiles/bytedance/pico4_controller";
    }
    if (lowerName.find("pico") != std::string::npos)
    {
        return "/interaction_profiles/bytedance/pico4_controller";
    }

    if (ContainsAny(lowerName, {"quest 3", "quest3", "touch plus"}))
    {
        return "/interaction_profiles/meta/touch_plus_controller";
    }
    if (ContainsAny(lowerName, {"quest 2", "quest2"}))
    {
        return "/interaction_profiles/meta/touch_controller_quest_2";
    }
    if (ContainsAny(lowerName, {"quest 1", "quest1", "rift s"}))
    {
        return "/interaction_profiles/meta/touch_controller_quest_1_rift_s";
    }
    if (ContainsAny(lowerName, {"quest", "oculus", "meta"}))
    {
        return "/interaction_profiles/oculus/touch_controller";
    }

    return "/interaction_profiles/oculus/touch_controller";
}

void AddProfileIfMissing(std::vector<std::string>& profiles, const std::string& profile)
{
    if (profile.empty())
    {
        return;
    }
    if (std::find(profiles.begin(), profiles.end(), profile) == profiles.end())
    {
        profiles.push_back(profile);
    }
}

glm::vec3 JointVec3(const InputManager::StreamingHandState& handState, size_t index)
{
    const glm::vec4& joint = handState.joints[index];
    return {joint.x, joint.y, joint.z};
}

glm::quat BuildTrackedHandOrientation(InputManager::Hand hand,
                                      const InputManager::StreamingHandState& handState)
{
    glm::vec3 palm = JointVec3(handState, XR_HAND_JOINT_PALM_EXT);
    glm::vec3 wrist = JointVec3(handState, XR_HAND_JOINT_WRIST_EXT);
    glm::vec3 indexBase = JointVec3(handState, XR_HAND_JOINT_INDEX_METACARPAL_EXT);
    glm::vec3 littleBase = JointVec3(handState, XR_HAND_JOINT_LITTLE_METACARPAL_EXT);

    glm::vec3 up = palm - wrist;
    if (glm::dot(up, up) < 1e-6f)
    {
        up = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    up = glm::normalize(up);

    glm::vec3 across = indexBase - littleBase;
    if (glm::dot(across, across) < 1e-6f)
    {
        across = glm::vec3(1.0f, 0.0f, 0.0f);
    }
    across = glm::normalize(across);
    if (hand == InputManager::Hand::Left)
    {
        across = -across;
    }

    glm::vec3 forward = glm::cross(across, up);
    if (glm::dot(forward, forward) < 1e-6f)
    {
        forward = glm::vec3(0.0f, 0.0f, -1.0f);
    }
    forward = glm::normalize(forward);
    across = glm::normalize(glm::cross(up, forward));

    glm::mat3 basis(across, up, forward);
    return glm::normalize(glm::quat_cast(basis));
}

XrPosef MakePose(const glm::vec3& position, const glm::quat& orientation)
{
    XrPosef pose{};
    pose.position = {position.x, position.y, position.z};
    pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
    return pose;
}

} // namespace

InputManager::InputManager() = default;

void InputManager::SetTrackingReceiver(TrackingReceiver* receiver)
{
    trackingReceiver_ = receiver;
    if (receiver == nullptr)
    {
        streamingControllerActive_.fill(false);
        for (auto& handState : streamingHands_)
        {
            handState.active = false;
        }
        streamingClientName_.clear();
        streamingControllerProfile_.clear();
    }
}

void InputManager::SetStreamingClientName(const std::string& clientName)
{
    streamingClientName_ = clientName;
    streamingControllerProfile_ = DetectStreamingControllerProfile(clientName);
    spdlog::info("InputManager: streaming client='{}' controller_profile='{}'",
                 streamingClientName_, streamingControllerProfile_);
}

glm::quat InputManager::GetHeadRotation() const
{
    return headQuat_;
}

void InputManager::Update(float deltaTime)
{
    (void)deltaTime;
    if (trackingReceiver_ != nullptr && trackingReceiver_->IsReceiving())
    {
        UpdateFromStreaming();
    }
}

void InputManager::UpdateFromStreaming()
{
    oxr::protocol::TrackingPacket packet;
    if (!trackingReceiver_->GetPredictedPose(packet) &&
        !trackingReceiver_->GetLatestPose(packet))
    {
        return;
    }

    // Apply head pose from headset tracking — store quaternion directly
    headPosition_ = glm::vec3(packet.headPosition[0],
                                packet.headPosition[1],
                                packet.headPosition[2]);

    headQuat_ = glm::quat(packet.headOrientation[3],  // w
                           packet.headOrientation[0],   // x
                           packet.headOrientation[1],   // y
                           packet.headOrientation[2]);  // z

    const bool leftControllerActive =
        (packet.trackingFlags & oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE) != 0;
    const bool rightControllerActive =
        (packet.trackingFlags & oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE) != 0;
    streamingControllerActive_[HandIndex(Hand::Left)] = leftControllerActive;
    streamingControllerActive_[HandIndex(Hand::Right)] = rightControllerActive;

    if (leftControllerActive)
    {
        leftControllerPos_ = glm::vec3(packet.leftControllerPos[0],
                                       packet.leftControllerPos[1],
                                       packet.leftControllerPos[2]);
        leftControllerRot_ = glm::quat(packet.leftControllerRot[3],
                                       packet.leftControllerRot[0],
                                       packet.leftControllerRot[1],
                                       packet.leftControllerRot[2]);
    }
    if (rightControllerActive)
    {
        rightControllerPos_ = glm::vec3(packet.rightControllerPos[0],
                                        packet.rightControllerPos[1],
                                        packet.rightControllerPos[2]);
        rightControllerRot_ = glm::quat(packet.rightControllerRot[3],
                                        packet.rightControllerRot[0],
                                        packet.rightControllerRot[1],
                                        packet.rightControllerRot[2]);
    }

    // Apply button/trigger states
    buttonState_ = packet.buttonState;
    leftTrigger_ = packet.leftTrigger;
    rightTrigger_ = packet.rightTrigger;
    leftGripValue_ = packet.leftGrip;
    rightGripValue_ = packet.rightGrip;
    leftThumbstick_ = {packet.leftThumbstick[0], packet.leftThumbstick[1]};
    rightThumbstick_ = {packet.rightThumbstick[0], packet.rightThumbstick[1]};
    leftGrab_ = packet.leftGrip > 0.5f;
    rightGrab_ = packet.rightGrip > 0.5f;
    menuClick_ = (packet.buttonState & oxr::protocol::BUTTON_MENU) != 0;
    streamingIpd_ = packet.ipd;
    streamingFov_[0] = packet.eyeFov[0];
    streamingFov_[1] = packet.eyeFov[1];
    streamingFov_[2] = packet.eyeFov[2];
    streamingFov_[3] = packet.eyeFov[3];

    for (size_t handIndex = 0; handIndex < 2; ++handIndex)
    {
        const bool isActive =
            (packet.trackingFlags &
             (handIndex == 0 ? oxr::protocol::TRACKING_FLAG_LEFT_HAND_ACTIVE
                             : oxr::protocol::TRACKING_FLAG_RIGHT_HAND_ACTIVE)) != 0;
        streamingHands_[handIndex].active = isActive;

        const auto& sourceJoints = handIndex == 0 ? packet.leftHandJoints : packet.rightHandJoints;
        for (size_t jointIndex = 0; jointIndex < oxr::protocol::HAND_JOINT_COUNT; ++jointIndex)
        {
            streamingHands_[handIndex].joints[jointIndex] = glm::vec4(
                sourceJoints[jointIndex][0],
                sourceJoints[jointIndex][1],
                sourceJoints[jointIndex][2],
                sourceJoints[jointIndex][3]);
        }
    }

    // Debug: log streaming pose periodically
    static uint32_t streamLogCounter = 0;
    if (++streamLogCounter % 270 == 0) // ~3 seconds
    {
        spdlog::info("InputManager: streaming pos=({:.3f}, {:.3f}, {:.3f}) "
                      "L=({:.3f},{:.3f},{:.3f}) R=({:.3f},{:.3f},{:.3f}) "
                      "trig={:.2f}/{:.2f} btn=0x{:x} flags=0x{:x}",
                      headPosition_.x, headPosition_.y, headPosition_.z,
                      leftControllerPos_.x, leftControllerPos_.y, leftControllerPos_.z,
                      rightControllerPos_.x, rightControllerPos_.y, rightControllerPos_.z,
                      leftTrigger_, rightTrigger_, buttonState_, packet.trackingFlags);
    }

    // Log Quest IPD/FOV on first packet for debugging
    static bool loggedOnce = false;
    if (!loggedOnce && packet.ipd > 0.0f)
    {
        spdlog::info("InputManager: Quest IPD={:.1f}mm FOV L={:.2f} R={:.2f} U={:.2f} D={:.2f} (rad), prediction={:.1f}ms",
                      packet.ipd * 1000.0f,
                      packet.eyeFov[0], packet.eyeFov[1],
                      packet.eyeFov[2], packet.eyeFov[3],
                      trackingReceiver_->GetPredictionHorizonMs());
        loggedOnce = true;
    }
}

XrPosef InputManager::GetHeadPose() const
{
    glm::quat rot = GetHeadRotation();

    XrPosef pose{};
    pose.orientation.x = rot.x;
    pose.orientation.y = rot.y;
    pose.orientation.z = rot.z;
    pose.orientation.w = rot.w;
    pose.position.x = headPosition_.x;
    pose.position.y = headPosition_.y;
    pose.position.z = headPosition_.z;
    return pose;
}

void InputManager::GetEyeViews(XrView* views, uint32_t viewCount) const
{
    if (viewCount < 2)
    {
        return;
    }

    glm::quat rot = GetHeadRotation();
    glm::vec3 right = rot * glm::vec3(1.0f, 0.0f, 0.0f);
    float ipd = (IsStreaming() && streamingIpd_ > 0.0f)
        ? streamingIpd_
        : DefaultIpd;
    float halfIpd = ipd * 0.5f;

    bool hasStreamingFov = IsStreaming() &&
        streamingFov_[0] < 0.0f && streamingFov_[1] > 0.0f &&
        streamingFov_[2] > 0.0f && streamingFov_[3] < 0.0f;

    for (uint32_t i = 0; i < 2; i++)
    {
        views[i].type = XR_TYPE_VIEW;
        views[i].next = nullptr;

        float sign = (i == 0) ? -1.0f : 1.0f;
        glm::vec3 eyePos = headPosition_ + right * (sign * halfIpd);

        views[i].pose.orientation.x = rot.x;
        views[i].pose.orientation.y = rot.y;
        views[i].pose.orientation.z = rot.z;
        views[i].pose.orientation.w = rot.w;
        views[i].pose.position.x = eyePos.x;
        views[i].pose.position.y = eyePos.y;
        views[i].pose.position.z = eyePos.z;

        // Compute aspect-ratio-correct FOV. fov_degrees is the vertical FOV;
        // horizontal FOV is derived from the texture aspect ratio so angular
        // pixels are square, matching what the Quest compositor expects.
        if (hasStreamingFov)
        {
            if (i == 0)
            {
                views[i].fov.angleLeft = streamingFov_[0];
                views[i].fov.angleRight = streamingFov_[1];
            }
            else
            {
                views[i].fov.angleLeft = -streamingFov_[1];
                views[i].fov.angleRight = -streamingFov_[0];
            }
            views[i].fov.angleUp = streamingFov_[2];
            views[i].fov.angleDown = streamingFov_[3];
        }
        else
        {
            const ConfigValues config = Config::Get().GetValues();
            float fovDeg = static_cast<float>(config.fovDegrees);
            float halfAngleV = fovDeg * 0.5f * 3.14159265f / 180.0f;
            constexpr float aspect = static_cast<float>(Instance::EyeWidth) /
                                      static_cast<float>(Instance::EyeHeight);
            float halfAngleH = std::atan(std::tan(halfAngleV) * aspect);
            views[i].fov.angleLeft = -halfAngleH;
            views[i].fov.angleRight = halfAngleH;
            views[i].fov.angleUp = halfAngleV;
            views[i].fov.angleDown = -halfAngleV;
        }
    }

    static bool loggedFov = false;
    if (!loggedFov)
    {
        spdlog::info("InputManager: Using {} FOV/IPD (L={:.2f} R={:.2f} U={:.2f} D={:.2f}, IPD={:.1f}mm)",
                      hasStreamingFov ? "streaming" : "default",
                      views[0].fov.angleLeft, views[0].fov.angleRight,
                      views[0].fov.angleUp, views[0].fov.angleDown,
                      ipd * 1000.0f);
        loggedFov = true;
    }
}

XrPosef InputManager::GetControllerPose(Hand hand) const
{
    const glm::vec3& pos = (hand == Hand::Left) ? leftControllerPos_ : rightControllerPos_;

    glm::quat rot = (hand == Hand::Left) ? leftControllerRot_ : rightControllerRot_;

    XrPosef pose{};
    pose.orientation.x = rot.x;
    pose.orientation.y = rot.y;
    pose.orientation.z = rot.z;
    pose.orientation.w = rot.w;
    pose.position.x = pos.x;
    pose.position.y = pos.y;
    pose.position.z = pos.z;
    return pose;
}

float InputManager::GetGrabValue(Hand hand) const
{
    if (IsHandTrackingActive(hand))
    {
        return GetTrackedGraspValue(hand);
    }

    return (hand == Hand::Left) ? leftGripValue_ : rightGripValue_;
}

bool InputManager::GetMenuClick() const
{
    return menuClick_;
}

float InputManager::GetTriggerValue(Hand hand) const
{
    if (IsHandTrackingActive(hand))
    {
        return GetTrackedPinchValue(hand);
    }

    return (hand == Hand::Left) ? leftTrigger_ : rightTrigger_;
}

XrVector2f InputManager::GetThumbstickValue(Hand hand) const
{
    return (hand == Hand::Left) ? leftThumbstick_ : rightThumbstick_;
}

const InputManager::AutomationHandState& InputManager::GetAutomationState(Hand hand) const
{
    return automationHands_[HandIndex(hand)];
}

InputManager::AutomationHandState& InputManager::GetAutomationState(Hand hand)
{
    return automationHands_[HandIndex(hand)];
}

void InputManager::SetAutomationInteractionProfile(Hand hand, const std::string& interactionProfile, bool isActive)
{
    auto& automation = GetAutomationState(hand);
    automation.hasExplicitActivity = true;
    automation.isActive = isActive;
    automation.interactionProfile = interactionProfile;
}

void InputManager::SetAutomationBoolean(Hand hand, const std::string& componentPath, bool state)
{
    GetAutomationState(hand).boolStates[componentPath] = state;
}

void InputManager::SetAutomationFloat(Hand hand, const std::string& componentPath, float state)
{
    GetAutomationState(hand).floatStates[componentPath] = state;
}

void InputManager::SetAutomationVector2f(Hand hand, const std::string& componentPath, XrVector2f state)
{
    GetAutomationState(hand).vector2fStates[componentPath] = state;
}

void InputManager::SetAutomationPose(Hand hand, const std::string& componentPath, const XrPosef& pose)
{
    GetAutomationState(hand).poseStates[componentPath] = pose;
}

bool InputManager::IsInputDeviceActive(Hand hand) const
{
    const auto& automation = GetAutomationState(hand);
    if (automation.hasExplicitActivity)
    {
        return automation.isActive;
    }

    return IsHandTrackingActive(hand) || IsControllerTrackingActive(hand);
}

bool InputManager::IsControllerTrackingActive(Hand hand) const
{
    if (IsStreaming())
    {
        return streamingControllerActive_[HandIndex(hand)];
    }

    return false;
}

bool InputManager::IsHandTrackingActive(Hand hand) const
{
    if (IsStreaming())
    {
        return streamingHands_[HandIndex(hand)].active;
    }

    return false;
}

std::string InputManager::GetCurrentInteractionProfile(Hand hand) const
{
    const auto& automation = GetAutomationState(hand);
    if (automation.hasExplicitActivity)
    {
        if (!automation.isActive)
        {
            return "";
        }
        if (!automation.interactionProfile.empty())
        {
            return automation.interactionProfile;
        }
    }

    if (IsHandTrackingActive(hand))
    {
        return "/interaction_profiles/ext/hand_interaction_ext";
    }

    if (IsControllerTrackingActive(hand))
    {
        return streamingControllerProfile_.empty()
            ? "/interaction_profiles/oculus/touch_controller"
            : streamingControllerProfile_;
    }

    if (IsStreaming())
    {
        return "";
    }

    return "/interaction_profiles/khr/simple_controller";
}

std::vector<std::string> InputManager::GetActiveInteractionProfiles(Hand hand) const
{
    std::vector<std::string> profiles;
    const std::string currentProfile = GetCurrentInteractionProfile(hand);
    AddProfileIfMissing(profiles, currentProfile);

    const auto& automation = GetAutomationState(hand);
    if (automation.hasExplicitActivity || currentProfile == "/interaction_profiles/ext/hand_interaction_ext")
    {
        return profiles;
    }

    if (IsControllerTrackingActive(hand))
    {
        if (currentProfile.find("/interaction_profiles/meta/") == 0 ||
            currentProfile == "/interaction_profiles/oculus/touch_controller")
        {
            AddProfileIfMissing(profiles, "/interaction_profiles/oculus/touch_controller");
        }
        AddProfileIfMissing(profiles, "/interaction_profiles/khr/simple_controller");
    }

    return profiles;
}

bool InputManager::GetButtonClick(Hand hand, const std::string& componentPath) const
{
    const auto& automation = GetAutomationState(hand);
    auto boolIt = automation.boolStates.find(componentPath);
    if (boolIt != automation.boolStates.end())
    {
        return boolIt->second;
    }

    if (componentPath == "menu/click")
    {
        return GetMenuClick();
    }
    if (componentPath == "select/click")
    {
        return GetGrabValue(hand) > 0.5f;
    }
    if (componentPath == "trigger/click")
    {
        return GetTriggerValue(hand) > 0.5f;
    }
    if (componentPath == "trigger/touch")
    {
        return GetTriggerValue(hand) > 0.01f;
    }
    if (componentPath == "squeeze/click")
    {
        return GetGrabValue(hand) > 0.5f;
    }
    if (componentPath == "thumbstick/click")
    {
        uint32_t mask = (hand == Hand::Left)
                            ? oxr::protocol::BUTTON_LEFT_THUMBSTICK
                            : oxr::protocol::BUTTON_RIGHT_THUMBSTICK;
        return (buttonState_ & mask) != 0;
    }
    if (componentPath == "thumbstick/touch" || componentPath == "thumbrest/touch")
    {
        XrVector2f thumbstick = GetThumbstickValue(hand);
        return std::fabs(thumbstick.x) > 0.01f || std::fabs(thumbstick.y) > 0.01f;
    }
    if (componentPath == "a/click" || componentPath == "a/touch")
    {
        return hand == Hand::Right && (buttonState_ & oxr::protocol::BUTTON_A) != 0;
    }
    if (componentPath == "b/click" || componentPath == "b/touch")
    {
        return hand == Hand::Right && (buttonState_ & oxr::protocol::BUTTON_B) != 0;
    }
    if (componentPath == "x/click" || componentPath == "x/touch")
    {
        return hand == Hand::Left && (buttonState_ & oxr::protocol::BUTTON_X) != 0;
    }
    if (componentPath == "y/click" || componentPath == "y/touch")
    {
        return hand == Hand::Left && (buttonState_ & oxr::protocol::BUTTON_Y) != 0;
    }
    if (componentPath == "pinch_ext/ready_ext" || componentPath == "grasp_ext/ready_ext" ||
        componentPath == "aim_activate_ext/ready_ext")
    {
        return IsHandTrackingActive(hand);
    }

    return false;
}

bool InputManager::GetBooleanComponent(Hand hand, const std::string& componentPath) const
{
    return GetButtonClick(hand, componentPath);
}

float InputManager::GetFloatComponent(Hand hand, const std::string& componentPath) const
{
    const auto& automation = GetAutomationState(hand);
    auto floatIt = automation.floatStates.find(componentPath);
    if (floatIt != automation.floatStates.end())
    {
        return floatIt->second;
    }

    if (componentPath == "select/value")
    {
        return GetGrabValue(hand);
    }
    if (componentPath == "trigger/value")
    {
        return GetTriggerValue(hand);
    }
    if (componentPath == "squeeze/value")
    {
        return GetGrabValue(hand);
    }
    if (componentPath == "thumbstick/x")
    {
        return GetThumbstickValue(hand).x;
    }
    if (componentPath == "thumbstick/y")
    {
        return GetThumbstickValue(hand).y;
    }
    if (componentPath == "pinch_ext/value" || componentPath == "grasp_ext/value" ||
        componentPath == "aim_activate_ext/value")
    {
        if (componentPath == "grasp_ext/value")
        {
            return GetTrackedGraspValue(hand);
        }
        return GetTrackedPinchValue(hand);
    }

    return GetBooleanComponent(hand, componentPath) ? 1.0f : 0.0f;
}

XrVector2f InputManager::GetVector2fComponent(Hand hand, const std::string& componentPath) const
{
    const auto& automation = GetAutomationState(hand);
    auto vecIt = automation.vector2fStates.find(componentPath);
    if (vecIt != automation.vector2fStates.end())
    {
        return vecIt->second;
    }

    if (componentPath == "thumbstick")
    {
        return GetThumbstickValue(hand);
    }

    return {0.0f, 0.0f};
}

XrPosef InputManager::GetPoseComponent(Hand hand, const std::string& componentPath) const
{
    const auto& automation = GetAutomationState(hand);
    auto poseIt = automation.poseStates.find(componentPath);
    if (poseIt != automation.poseStates.end())
    {
        return poseIt->second;
    }

    if (componentPath == "grip/pose" || componentPath == "aim/pose")
    {
        if (IsHandTrackingActive(hand))
        {
            return GetTrackedHandPose(hand, componentPath);
        }
        return GetControllerPose(hand);
    }

    if (componentPath == "pinch_ext/pose" || componentPath == "poke_ext/pose")
    {
        if (IsHandTrackingActive(hand))
        {
            return GetTrackedHandPose(hand, componentPath);
        }
        XrPosef pose = GetControllerPose(hand);
        pose.position.z -= 0.05f;
        return pose;
    }

    return GetControllerPose(hand);
}

float InputManager::GetTrackedPinchValue(Hand hand) const
{
    if (!IsHandTrackingActive(hand))
    {
        return 0.0f;
    }

    const auto& handState = streamingHands_[HandIndex(hand)];
    glm::vec3 thumbTip = JointVec3(handState, XR_HAND_JOINT_THUMB_TIP_EXT);
    glm::vec3 indexTip = JointVec3(handState, XR_HAND_JOINT_INDEX_TIP_EXT);
    float distance = glm::length(thumbTip - indexTip);
    constexpr float ClosedDistance = 0.015f;
    constexpr float OpenDistance = 0.065f;
    float normalized = 1.0f - std::clamp((distance - ClosedDistance) / (OpenDistance - ClosedDistance),
                                         0.0f, 1.0f);
    return normalized;
}

float InputManager::GetTrackedGraspValue(Hand hand) const
{
    if (!IsHandTrackingActive(hand))
    {
        return 0.0f;
    }

    const auto& handState = streamingHands_[HandIndex(hand)];
    glm::vec3 palm = JointVec3(handState, XR_HAND_JOINT_PALM_EXT);
    constexpr std::array<uint32_t, 4> FingerTips = {
        XR_HAND_JOINT_INDEX_TIP_EXT,
        XR_HAND_JOINT_MIDDLE_TIP_EXT,
        XR_HAND_JOINT_RING_TIP_EXT,
        XR_HAND_JOINT_LITTLE_TIP_EXT,
    };

    float total = 0.0f;
    for (uint32_t joint : FingerTips)
    {
        float distance = glm::length(JointVec3(handState, joint) - palm);
        constexpr float ClosedDistance = 0.035f;
        constexpr float OpenDistance = 0.110f;
        total += 1.0f - std::clamp((distance - ClosedDistance) / (OpenDistance - ClosedDistance),
                                   0.0f, 1.0f);
    }

    return total / static_cast<float>(FingerTips.size());
}

XrPosef InputManager::GetTrackedHandPose(Hand hand, const std::string& componentPath) const
{
    if (!IsHandTrackingActive(hand))
    {
        return GetControllerPose(hand);
    }

    const auto& handState = streamingHands_[HandIndex(hand)];
    glm::quat orientation = BuildTrackedHandOrientation(hand, handState);

    if (componentPath == "poke_ext/pose")
    {
        return MakePose(JointVec3(handState, XR_HAND_JOINT_INDEX_TIP_EXT), orientation);
    }
    if (componentPath == "pinch_ext/pose")
    {
        glm::vec3 thumbTip = JointVec3(handState, XR_HAND_JOINT_THUMB_TIP_EXT);
        glm::vec3 indexTip = JointVec3(handState, XR_HAND_JOINT_INDEX_TIP_EXT);
        return MakePose((thumbTip + indexTip) * 0.5f, orientation);
    }
    if (componentPath == "aim/pose")
    {
        return MakePose(JointVec3(handState, XR_HAND_JOINT_INDEX_PROXIMAL_EXT), orientation);
    }

    return MakePose(JointVec3(handState, XR_HAND_JOINT_PALM_EXT), orientation);
}

void InputManager::GenerateHandJoints(Hand hand, const glm::vec3& palmPos, const glm::quat& palmRot,
                                       XrHandJointLocationEXT* joints, uint32_t jointCount) const
{
    if (jointCount < 26)
    {
        return;
    }

    constexpr XrSpaceLocationFlags validFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                                 XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                                 XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                                                 XR_SPACE_LOCATION_POSITION_TRACKED_BIT;

    // Helper to set a joint
    auto setJoint = [&](int index, const glm::vec3& offset, float radius)
    {
        glm::vec3 worldPos = palmPos + palmRot * offset;
        joints[index].locationFlags = validFlags;
        joints[index].pose.orientation = {palmRot.x, palmRot.y, palmRot.z, palmRot.w};
        joints[index].pose.position = {worldPos.x, worldPos.y, worldPos.z};
        joints[index].radius = radius;
    };

    float side = (hand == Hand::Left) ? 1.0f : -1.0f;

    // Joint indices (from XR_HAND_JOINT_PALM_EXT = 0 to LITTLE_TIP = 25)
    // 0: Palm
    setJoint(0, {0, 0, 0}, 0.025f);
    // 1: Wrist
    setJoint(1, {0, 0, 0.08f}, 0.02f);

    // Finger base X offsets from palm center
    float fingerX[5] = {
        side * 0.03f,  // Thumb
        side * 0.02f,  // Index
        0.0f,          // Middle
        side * -0.02f, // Ring
        side * -0.04f, // Little
    };

    // Thumb (joints 2-5): metacarpal, proximal, distal, tip
    float thumbZ[4] = {0.0f, -0.025f, -0.045f, -0.06f};
    float thumbX[4] = {side * 0.02f, side * 0.035f, side * 0.045f, side * 0.05f};
    for (int j = 0; j < 4; j++)
    {
        setJoint(2 + j, {thumbX[j], 0, thumbZ[j]}, 0.01f);
    }

    // Index through Little (4 fingers × 5 joints each: metacarpal, proximal, intermediate, distal, tip)
    for (int finger = 0; finger < 4; finger++)
    {
        int baseIndex = 6 + finger * 5;
        float baseX = fingerX[finger + 1];
        float jointZ[5] = {-0.02f, -0.05f, -0.07f, -0.085f, -0.095f};
        float radii[5] = {0.008f, 0.007f, 0.006f, 0.006f, 0.005f};

        for (int j = 0; j < 5; j++)
        {
            setJoint(baseIndex + j, {baseX, 0, jointZ[j]}, radii[j]);
        }
    }
}

void InputManager::GetHandJointLocations(Hand hand, XrHandJointLocationEXT* joints, uint32_t jointCount) const
{
    if (IsStreaming() && IsHandTrackingActive(hand) &&
        jointCount >= oxr::protocol::HAND_JOINT_COUNT)
    {
        constexpr XrSpaceLocationFlags validFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                                    XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                                    XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                                                    XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        const auto& handState = streamingHands_[HandIndex(hand)];
        glm::quat orientation = BuildTrackedHandOrientation(hand, handState);
        for (uint32_t i = 0; i < oxr::protocol::HAND_JOINT_COUNT; ++i)
        {
            const glm::vec4& joint = handState.joints[i];
            joints[i].locationFlags = validFlags;
            joints[i].pose = MakePose(glm::vec3(joint.x, joint.y, joint.z), orientation);
            joints[i].radius = joint.w;
        }
        return;
    }

    XrPosef controllerPose = GetControllerPose(hand);
    glm::vec3 pos(controllerPose.position.x, controllerPose.position.y, controllerPose.position.z);
    glm::quat rot(controllerPose.orientation.w, controllerPose.orientation.x,
                   controllerPose.orientation.y, controllerPose.orientation.z);
    GenerateHandJoints(hand, pos, rot, joints, jointCount);
}
