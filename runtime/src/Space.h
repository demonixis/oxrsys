// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>
#include <cstdint>

class InputManager;
class Session;

struct SpaceWorldPose
{
    XrPosef pose{};
    bool active = true;
};

// Express a world-space pose relative to a world-space base pose.
XrPosef PoseRelativeTo(const XrPosef& worldPose, const XrPosef& baseWorldPose);

class Space
{
public:
    enum class Type
    {
        Reference,
        Action,
    };

    // Reference space constructor
    Space(Session* session, Type type, XrReferenceSpaceType refType, const XrPosef& poseInSpace);

    // Action space constructor
    Space(Session* session, XrAction action, XrPath subactionPath, const XrPosef& poseInSpace);

    ~Space();

    uint64_t GetHandle() const
    {
        return handle_;
    }

    Session* GetSession() const
    {
        return session_;
    }

    Type GetType() const
    {
        return type_;
    }

    XrReferenceSpaceType GetReferenceSpaceType() const
    {
        return referenceSpaceType_;
    }

    const XrPosef& GetPoseInSpace() const
    {
        return poseInSpace_;
    }

    XrPath GetSubactionPath() const
    {
        return subactionPath_;
    }

    XrAction GetAction() const
    {
        return action_;
    }

    XrResult LocateSpace(Space* baseSpace, XrTime time, XrSpaceLocation* location);

    // Pose of this space's origin in client STAGE / world coordinates, including
    // the space's own poseInReferenceSpace offset.
    SpaceWorldPose PoseInWorld(const InputManager& inputManager) const;

private:
    uint64_t handle_ = 0;
    Session* session_;
    Type type_;
    XrReferenceSpaceType referenceSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrPosef poseInSpace_;
    XrAction action_ = XR_NULL_HANDLE;
    XrPath subactionPath_ = XR_NULL_PATH;
};
