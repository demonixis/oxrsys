// SPDX-License-Identifier: MPL-2.0

#include "Space.h"
#include "Session.h"
#include "Runtime.h"
#include "InputManager.h"
#include "ActionSet.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

Space::Space(Session* session, Type type, XrReferenceSpaceType refType, const XrPosef& poseInSpace)
    : session_(session), type_(type), referenceSpaceType_(refType), poseInSpace_(poseInSpace)
{
    Runtime::Get().RegisterHandle(handle_, this);
}

Space::Space(Session* session, XrAction action, XrPath subactionPath, const XrPosef& poseInSpace)
    : session_(session), type_(Type::Action), poseInSpace_(poseInSpace), action_(action), subactionPath_(subactionPath)
{
    Runtime::Get().RegisterHandle(handle_, this);
}

Space::~Space()
{
    Runtime::Get().RemoveHandle(handle_);
}

static glm::quat ToGlm(const XrQuaternionf& q)
{
    return glm::quat(q.w, q.x, q.y, q.z);
}

static glm::vec3 ToGlm(const XrVector3f& v)
{
    return glm::vec3(v.x, v.y, v.z);
}

static XrQuaternionf ToXr(const glm::quat& q)
{
    return {q.x, q.y, q.z, q.w};
}

static XrVector3f ToXr(const glm::vec3& v)
{
    return {v.x, v.y, v.z};
}

static XrSpaceVelocity* FindSpaceVelocity(void* next)
{
    auto* current = reinterpret_cast<XrBaseOutStructure*>(next);
    while (current != nullptr)
    {
        if (current->type == XR_TYPE_SPACE_VELOCITY)
        {
            return reinterpret_cast<XrSpaceVelocity*>(current);
        }
        current = current->next;
    }
    return nullptr;
}

static InputManager::Hand HandFromPath(XrPath subactionPath)
{
    std::string pathStr = Runtime::Get().GetPathString(subactionPath);
    if (pathStr.find("right") != std::string::npos)
    {
        return InputManager::Hand::Right;
    }
    return InputManager::Hand::Left;
}

static InputManager::Hand HandFromBindingPath(const std::string& bindingPath)
{
    if (bindingPath.find("/user/hand/right") != std::string::npos)
    {
        return InputManager::Hand::Right;
    }
    return InputManager::Hand::Left;
}

static std::string ComponentFromBindingPath(const std::string& bindingPath)
{
    size_t inputPos = bindingPath.find("/input/");
    if (inputPos == std::string::npos)
    {
        return "";
    }
    return bindingPath.substr(inputPos + 7);
}

XrPosef PoseRelativeTo(const XrPosef& worldPose, const XrPosef& baseWorldPose)
{
    const glm::quat baseRotInv = glm::inverse(ToGlm(baseWorldPose.orientation));
    const glm::vec3 relativePosition =
        baseRotInv * (ToGlm(worldPose.position) - ToGlm(baseWorldPose.position));
    const glm::quat relativeRotation = baseRotInv * ToGlm(worldPose.orientation);

    XrPosef pose{};
    pose.orientation = ToXr(relativeRotation);
    pose.position = ToXr(relativePosition);
    return pose;
}

SpaceWorldPose Space::PoseInWorld(const InputManager& inputManager) const
{
    SpaceWorldPose result{};
    result.pose.orientation = {0, 0, 0, 1};
    result.pose.position = {0, 0, 0};
    result.active = true;

    if (type_ == Type::Reference)
    {
        switch (referenceSpaceType_)
        {
            case XR_REFERENCE_SPACE_TYPE_VIEW:
                result.pose = inputManager.GetHeadPose();
                break;
            case XR_REFERENCE_SPACE_TYPE_LOCAL:
            case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
            case XR_REFERENCE_SPACE_TYPE_STAGE:
                result.pose = inputManager.GetReferenceSpacePose(referenceSpaceType_);
                break;
            default:
                break;
        }
    }
    else if (type_ == Type::Action)
    {
        auto* action = Runtime::Get().FromHandle<ActionState>(
            reinterpret_cast<uint64_t>(action_));
        std::string poseBindingPath;
        std::string poseProfilePath;
        bool poseActive = false;

        if (action != nullptr)
        {
            const auto& data = action->GetSubactionData(subactionPath_);
            poseActive = data.poseActive;
            poseBindingPath = Runtime::Get().GetPathString(data.poseSourcePath);
            poseProfilePath = data.poseSourceProfile;

            if (poseBindingPath.empty() || !poseActive)
            {
                const auto& fallbackData = action->GetSubactionData(XR_NULL_PATH);
                poseActive = fallbackData.poseActive;
                poseBindingPath = Runtime::Get().GetPathString(fallbackData.poseSourcePath);
                poseProfilePath = fallbackData.poseSourceProfile;
            }
        }

        result.active = poseActive;
        if (poseActive && !poseBindingPath.empty())
        {
            InputManager::Hand hand = HandFromBindingPath(poseBindingPath);
            result.pose = inputManager.GetPoseComponentForProfile(
                hand, ComponentFromBindingPath(poseBindingPath), poseProfilePath);
        }
        else
        {
            InputManager::Hand hand = HandFromPath(subactionPath_);
            result.pose = inputManager.GetControllerPose(hand);
        }
    }

    // Apply the space's offset pose
    const XrPosef& offset = poseInSpace_;
    glm::quat worldRot = ToGlm(result.pose.orientation);
    glm::vec3 worldPos = ToGlm(result.pose.position);
    glm::quat offsetRot = ToGlm(offset.orientation);
    glm::vec3 offsetPos = ToGlm(offset.position);

    glm::quat finalRot = worldRot * offsetRot;
    glm::vec3 finalPos = worldPos + worldRot * offsetPos;

    result.pose.orientation = ToXr(finalRot);
    result.pose.position = ToXr(finalPos);
    return result;
}

XrResult Space::LocateSpace(Space* baseSpace, XrTime time, XrSpaceLocation* location)
{
    if (location == nullptr || baseSpace == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    if (baseSpace->GetSession() != session_)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    const InputManager& inputManager = session_->GetInputManager();

    const SpaceWorldPose thisPose = PoseInWorld(inputManager);
    const SpaceWorldPose basePose = baseSpace->PoseInWorld(inputManager);

    location->type = XR_TYPE_SPACE_LOCATION;
    location->pose = PoseRelativeTo(thisPose.pose, basePose.pose);
    location->locationFlags = (thisPose.active && basePose.active)
        ? XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
              XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT
        : 0;

    if (XrSpaceVelocity* velocity = FindSpaceVelocity(location->next))
    {
        velocity->velocityFlags = 0;
        velocity->linearVelocity = {0.0f, 0.0f, 0.0f};
        velocity->angularVelocity = {0.0f, 0.0f, 0.0f};
    }

    return XR_SUCCESS;
}
