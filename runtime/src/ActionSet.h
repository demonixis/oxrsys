// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

struct AggregatedActionState
{
    bool isActive = false;
    bool boolValue = false;
    float floatValue = 0.0f;
    XrVector2f vector2fValue = {0.0f, 0.0f};
    bool poseActive = false;
    XrPath poseSourcePath = XR_NULL_PATH;
    std::string poseSourceProfile;
    std::vector<XrPath> boundSources;
};

class ActionSetState
{
public:
    ActionSetState(const XrActionSetCreateInfo* createInfo);
    ~ActionSetState();

    uint64_t GetHandle() const
    {
        return handle_;
    }

    const std::string& GetName() const
    {
        return name_;
    }

    const std::string& GetLocalizedName() const
    {
        return localizedName_;
    }

private:
    uint64_t handle_ = 0;
    std::string name_;
    std::string localizedName_;
};

class ActionState
{
public:
    struct SubactionData
    {
        bool isActive = false;
        bool boolValue = false;
        bool boolChanged = false;
        float floatValue = 0.0f;
        bool floatChanged = false;
        XrVector2f vector2fValue = {0.0f, 0.0f};
        bool vector2fChanged = false;
        bool poseActive = false;
        XrPath poseSourcePath = XR_NULL_PATH;
        std::string poseSourceProfile;
        XrTime lastChangeTime = 0;
        std::vector<XrPath> boundSources;
    };

    ActionState(ActionSetState* actionSet, const XrActionCreateInfo* createInfo);
    ~ActionState();

    uint64_t GetHandle() const
    {
        return handle_;
    }

    XrActionType GetType() const
    {
        return type_;
    }

    const std::string& GetName() const
    {
        return name_;
    }

    const std::string& GetLocalizedName() const
    {
        return localizedName_;
    }

    ActionSetState* GetActionSet() const
    {
        return actionSet_;
    }

    const std::vector<XrPath>& GetSubactionPaths() const
    {
        return subactionPaths_;
    }

    std::vector<XrPath> GetResolvedSubactionPaths() const;
    SubactionData& GetSubactionData(XrPath subactionPath);
    const SubactionData& GetSubactionData(XrPath subactionPath) const;
    void ApplySyncState(XrPath subactionPath, const AggregatedActionState* aggregatedState, XrTime syncTime);
    std::vector<XrPath> GetBoundSources() const;

private:
    uint64_t handle_ = 0;
    ActionSetState* actionSet_;
    std::string name_;
    std::string localizedName_;
    XrActionType type_;
    std::vector<XrPath> subactionPaths_;

    // Key: XrPath (XR_NULL_PATH = 0 for actions without subaction paths)
    mutable std::unordered_map<uint64_t, SubactionData> subactionData_;
};
