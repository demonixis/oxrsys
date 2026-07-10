// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <array>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

#include <oxrsys/protocol/Protocol.h>

class TrackingReceiver;

class InputManager
{
public:
    enum class InputMode
    {
        Controller,
        HandTracking,
    };

    enum class Hand : int
    {
        Left = 0,
        Right = 1,
    };

    struct StreamingHandState
    {
        bool active = false;
        std::array<glm::vec4, oxr::protocol::HAND_JOINT_COUNT> joints = {};
    };

    InputManager();

    // Set the tracking receiver to read poses from (streaming client)
    void SetTrackingReceiver(TrackingReceiver* receiver);
    bool IsStreaming() const { return trackingReceiver_ != nullptr; }
    // Timestamp of the tracking sample the current frame's poses came from.
    int64_t GetLastTrackingSampleTimestampNs() const { return lastTrackingSampleNs_.load(); }

    // Per-frame update
    void Update(float deltaTime);

    // Head pose
    XrPosef GetHeadPose() const;
    void GetEyeViews(XrView* views, uint32_t viewCount) const;

    // Controller poses (world space)
    XrPosef GetControllerPose(Hand hand) const;
    XrPosef GetControllerAimPose(Hand hand) const;

    // Hand tracking joints (26 joints, world space relative to baseSpace)
    void GetHandJointLocations(Hand hand, XrHandJointLocationEXT* joints, uint32_t jointCount) const;

    // Button states
    float GetGrabValue(Hand hand) const;
    bool GetMenuClick() const;
    float GetTriggerValue(Hand hand) const;
    XrVector2f GetThumbstickValue(Hand hand) const;
    bool GetButtonClick(Hand hand, const std::string& componentPath) const;
    bool IsInputDeviceActive(Hand hand) const;
    bool IsControllerTrackingActive(Hand hand) const;
    bool IsHandTrackingActive(Hand hand) const;
    std::string GetCurrentInteractionProfile(Hand hand) const;
    std::vector<std::string> GetCurrentInteractionProfileCandidates(Hand hand) const;
    std::vector<std::string> GetActiveInteractionProfiles(Hand hand) const;
    bool GetBooleanComponent(Hand hand, const std::string& componentPath) const;
    float GetFloatComponent(Hand hand, const std::string& componentPath) const;
    XrVector2f GetVector2fComponent(Hand hand, const std::string& componentPath) const;
    XrPosef GetPoseComponent(Hand hand, const std::string& componentPath) const;
    bool GetBooleanComponentForProfile(Hand hand, const std::string& componentPath,
                                       const std::string& profilePath) const;
    float GetFloatComponentForProfile(Hand hand, const std::string& componentPath,
                                      const std::string& profilePath) const;
    XrVector2f GetVector2fComponentForProfile(Hand hand, const std::string& componentPath,
                                              const std::string& profilePath) const;
    XrPosef GetPoseComponentForProfile(Hand hand, const std::string& componentPath,
                                       const std::string& profilePath) const;
    void SetStreamingClientName(const std::string& clientName);
    // Dev/testing only: fabricate khr/simple_controller profiles when no client is
    // connected (see ConfigValues::simpleControllerFallback for why this is off by default).
    void SetSimpleControllerFallback(bool enabled) { simpleControllerFallback_ = enabled; }
    // True once a controller has been seen on the current streaming connection. The
    // resolved profile stays bound (sticky) until the client disconnects, even while
    // the controllers are idle (system overlay, controllers asleep) — real runtimes
    // keep the profile bound and only drop action isActive.
    bool HasResolvedControllerProfile() const { return !resolvedControllerProfile_.empty(); }

    // Conformance automation overrides
    void SetAutomationInteractionProfile(Hand hand, const std::string& interactionProfile, bool isActive);
    void SetAutomationBoolean(Hand hand, const std::string& componentPath, bool state);
    void SetAutomationFloat(Hand hand, const std::string& componentPath, float state);
    void SetAutomationVector2f(Hand hand, const std::string& componentPath, XrVector2f state);
    void SetAutomationPose(Hand hand, const std::string& componentPath, const XrPosef& pose);

    // Mode
    InputMode GetInputMode() const
    {
        return mode_;
    }

private:
    struct AutomationHandState
    {
        bool hasExplicitActivity = false;
        bool isActive = false;
        std::string interactionProfile;
        std::unordered_map<std::string, bool> boolStates;
        std::unordered_map<std::string, float> floatStates;
        std::unordered_map<std::string, XrVector2f> vector2fStates;
        std::unordered_map<std::string, XrPosef> poseStates;
    };

    glm::quat GetHeadRotation() const;
    void UpdateFromStreaming();
    const AutomationHandState& GetAutomationState(Hand hand) const;
    AutomationHandState& GetAutomationState(Hand hand);
    void GenerateHandJoints(Hand hand, const glm::vec3& palmPos, const glm::quat& palmRot,
                             XrHandJointLocationEXT* joints, uint32_t jointCount) const;
    XrPosef GetTrackedHandPose(Hand hand, const std::string& componentPath) const;
    float GetTrackedPinchValue(Hand hand) const;
    float GetTrackedGraspValue(Hand hand) const;

    TrackingReceiver* trackingReceiver_ = nullptr;
    std::atomic<int64_t> lastTrackingSampleNs_{0};

    // Head state (quaternion from streaming client)
    glm::quat headQuat_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // w,x,y,z
    glm::quat leftControllerRot_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::quat rightControllerRot_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 headPosition_ = {0.0f, 1.6f, 0.0f};

    // Streaming controller state
    float leftTrigger_ = 0.0f;
    float rightTrigger_ = 0.0f;
    float leftGripValue_ = 0.0f;
    float rightGripValue_ = 0.0f;
    XrVector2f leftThumbstick_ = {0.0f, 0.0f};
    XrVector2f rightThumbstick_ = {0.0f, 0.0f};
    uint32_t buttonState_ = 0;
    std::array<bool, 2> streamingControllerActive_ = {false, false};
    std::string streamingClientName_;
    std::string streamingControllerProfile_;
    // Sticky per-client interaction profile: set for both hands when either controller
    // first activates, cleared only on disconnect. See HasResolvedControllerProfile().
    std::string resolvedControllerProfile_;
    bool simpleControllerFallback_ = false;

    // Controller positions (world space offsets)
    glm::vec3 leftControllerPos_ = {-0.2f, 1.3f, -0.4f};
    glm::vec3 rightControllerPos_ = {0.2f, 1.3f, -0.4f};

    // Aim (pointer) pose from the streaming client — distinct from grip. Used for
    // menu lasers. leftAimValid_/rightAimValid_ is false until an aim pose arrives,
    // in which case GetControllerAimPose falls back to the grip pose.
    glm::vec3 leftAimPos_ = {-0.2f, 1.3f, -0.4f};
    glm::vec3 rightAimPos_ = {0.2f, 1.3f, -0.4f};
    glm::quat leftAimRot_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::quat rightAimRot_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    bool leftAimValid_ = false;
    bool rightAimValid_ = false;

    // Button states
    bool leftGrab_ = false;
    bool rightGrab_ = false;
    bool menuClick_ = false;

    InputMode mode_ = InputMode::Controller;
    AutomationHandState automationHands_[2];
    StreamingHandState streamingHands_[2];

    // Streaming eye data (received from headset)
    float streamingIpd_ = 0.0f;         // 0 = use default
    float streamingFov_[4] = {};         // left, right, up, down (radians), 0 = use default

    static constexpr float DefaultIpd = 0.063f;
    static constexpr float DefaultFovAngle = 1.7453f; // ~100 degrees
};

class Instance;

// Resolve the app-visible interaction profile for a hand: the candidate list is
// filtered through the instance version / enabled extensions (the same
// IsKnownInteractionProfilePath rules), falling back to oculus/touch. This is
// exactly the value xrGetCurrentInteractionProfile returns, and it can differ
// from the raw InputManager profile (e.g. ext/hand_interaction_ext when the app
// never enabled XR_EXT_hand_interaction). Shared by the getter and the Session
// interaction-profile-changed debounce so the two can never diverge.
std::string SelectCurrentInteractionProfileForInstance(
    const Instance* instance, const InputManager& inputManager, InputManager::Hand hand);
