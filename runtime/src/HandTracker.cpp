// SPDX-License-Identifier: MPL-2.0

#include "HandTracker.h"
#include "Session.h"
#include "Runtime.h"
#include "InputManager.h"
#include "Space.h"

namespace
{

XrHandJointVelocitiesEXT* FindJointVelocities(XrHandJointLocationsEXT* locations)
{
    auto* next = reinterpret_cast<XrBaseOutStructure*>(locations->next);
    while (next != nullptr)
    {
        if (next->type == XR_TYPE_HAND_JOINT_VELOCITIES_EXT)
        {
            return reinterpret_cast<XrHandJointVelocitiesEXT*>(next);
        }
        next = next->next;
    }

    return nullptr;
}

void ClearJointLocations(XrHandJointLocationsEXT* locations)
{
    if (locations == nullptr || locations->jointLocations == nullptr)
    {
        return;
    }

    for (uint32_t i = 0; i < locations->jointCount; ++i)
    {
        locations->jointLocations[i].locationFlags = 0;
        locations->jointLocations[i].pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        locations->jointLocations[i].pose.position = {0.0f, 0.0f, 0.0f};
        locations->jointLocations[i].radius = 0.0f;
    }
}

void ClearJointVelocities(XrHandJointVelocitiesEXT* velocities)
{
    if (velocities == nullptr || velocities->jointVelocities == nullptr)
    {
        return;
    }

    for (uint32_t i = 0; i < velocities->jointCount; ++i)
    {
        velocities->jointVelocities[i].velocityFlags = 0;
        velocities->jointVelocities[i].linearVelocity = {0.0f, 0.0f, 0.0f};
        velocities->jointVelocities[i].angularVelocity = {0.0f, 0.0f, 0.0f};
    }
}

} // namespace

HandTracker::HandTracker(Session* session, XrHandEXT hand)
    : session_(session), hand_(hand)
{
    Runtime::Get().RegisterHandle(handle_, this);
}

HandTracker::~HandTracker()
{
    Runtime::Get().RemoveHandle(handle_);
}

XrResult HandTracker::LocateHandJoints(XrSpace baseSpace, XrTime /*time*/,
                                        XrHandJointLocationsEXT* locations)
{
    if (locations == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    XrHandJointVelocitiesEXT* velocities = FindJointVelocities(locations);
    if (locations->jointCount != XR_HAND_JOINT_COUNT_EXT || locations->jointLocations == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (velocities != nullptr &&
        (velocities->jointCount != XR_HAND_JOINT_COUNT_EXT || velocities->jointVelocities == nullptr))
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    auto* base = Runtime::Get().FromHandle<Space>(reinterpret_cast<uint64_t>(baseSpace));
    if (base == nullptr || base->GetSession() != session_)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    const InputManager& inputManager = session_->GetInputManager();

    InputManager::Hand hand = (hand_ == XR_HAND_LEFT_EXT)
                                  ? InputManager::Hand::Left
                                  : InputManager::Hand::Right;

    if (!inputManager.IsHandTrackingActive(hand))
    {
        locations->isActive = XR_FALSE;
        ClearJointLocations(locations);
        ClearJointVelocities(velocities);
        return XR_SUCCESS;
    }

    locations->isActive = XR_TRUE;
    inputManager.GetHandJointLocations(hand, locations->jointLocations, locations->jointCount);

    const SpaceWorldPose baseWorld = base->PoseInWorld(inputManager);
    if (!baseWorld.active)
    {
        locations->isActive = XR_FALSE;
        ClearJointLocations(locations);
        ClearJointVelocities(velocities);
        return XR_SUCCESS;
    }

    for (uint32_t i = 0; i < locations->jointCount; ++i)
    {
        locations->jointLocations[i].pose =
            PoseRelativeTo(locations->jointLocations[i].pose, baseWorld.pose);
    }
    ClearJointVelocities(velocities);

    return XR_SUCCESS;
}
