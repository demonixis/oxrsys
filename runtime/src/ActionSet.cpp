// SPDX-License-Identifier: MPL-2.0

#include "ActionSet.h"
#include "Runtime.h"
#include <algorithm>
#include <cmath>

ActionSetState::ActionSetState(const XrActionSetCreateInfo* createInfo)
{
    name_ = createInfo->actionSetName;
    localizedName_ = createInfo->localizedActionSetName;
    Runtime::Get().RegisterHandle(handle_, this);
}

ActionSetState::~ActionSetState()
{
    Runtime::Get().RemoveHandle(handle_);
}

ActionState::ActionState(ActionSetState* actionSet, const XrActionCreateInfo* createInfo)
    : actionSet_(actionSet)
{
    name_ = createInfo->actionName;
    localizedName_ = createInfo->localizedActionName;
    type_ = createInfo->actionType;
    if (createInfo->countSubactionPaths > 0 && createInfo->subactionPaths != nullptr)
    {
        subactionPaths_.assign(createInfo->subactionPaths,
                                createInfo->subactionPaths + createInfo->countSubactionPaths);
    }
    Runtime::Get().RegisterHandle(handle_, this);
}

ActionState::~ActionState()
{
    Runtime::Get().RemoveHandle(handle_);
}

ActionState::SubactionData& ActionState::GetSubactionData(XrPath subactionPath)
{
    return subactionData_[static_cast<uint64_t>(subactionPath)];
}

const ActionState::SubactionData& ActionState::GetSubactionData(XrPath subactionPath) const
{
    return subactionData_[static_cast<uint64_t>(subactionPath)];
}

std::vector<XrPath> ActionState::GetResolvedSubactionPaths() const
{
    if (subactionPaths_.empty())
    {
        return {XR_NULL_PATH};
    }
    return subactionPaths_;
}

void ActionState::ApplySyncState(XrPath subactionPath, const AggregatedActionState* aggregatedState,
                                  XrTime syncTime)
{
    auto& data = GetSubactionData(subactionPath);

    bool oldBool = data.boolValue;
    float oldFloat = data.floatValue;
    XrVector2f oldVector = data.vector2fValue;
    bool oldPoseActive = data.poseActive;
    XrPath oldPoseSourcePath = data.poseSourcePath;
    std::string oldPoseSourceProfile = data.poseSourceProfile;

    if (aggregatedState != nullptr)
    {
        data.isActive = aggregatedState->isActive;
        data.boolValue = aggregatedState->boolValue;
        data.floatValue = aggregatedState->floatValue;
        data.vector2fValue = aggregatedState->vector2fValue;
        data.poseActive = aggregatedState->poseActive;
        data.poseSourcePath = aggregatedState->poseSourcePath;
        data.poseSourceProfile = aggregatedState->poseSourceProfile;
        data.boundSources = aggregatedState->boundSources;
    }
    else
    {
        data.isActive = false;
        data.boolValue = false;
        data.floatValue = 0.0f;
        data.vector2fValue = {0.0f, 0.0f};
        data.poseActive = false;
        data.poseSourcePath = XR_NULL_PATH;
        data.poseSourceProfile.clear();
        data.boundSources.clear();
    }

    data.boolChanged = (data.boolValue != oldBool);
    data.floatChanged = std::fabs(data.floatValue - oldFloat) > 0.0001f;
    data.vector2fChanged = (std::fabs(data.vector2fValue.x - oldVector.x) > 0.0001f) ||
                           (std::fabs(data.vector2fValue.y - oldVector.y) > 0.0001f);

    bool poseChanged = (data.poseActive != oldPoseActive) ||
                       (data.poseSourcePath != oldPoseSourcePath) ||
                       (data.poseSourceProfile != oldPoseSourceProfile);
    if (data.boolChanged || data.floatChanged || data.vector2fChanged || poseChanged)
    {
        data.lastChangeTime = syncTime;
    }
}

std::vector<XrPath> ActionState::GetBoundSources() const
{
    std::vector<XrPath> sources;

    for (XrPath subactionPath : GetResolvedSubactionPaths())
    {
        const auto& data = GetSubactionData(subactionPath);
        sources.insert(sources.end(), data.boundSources.begin(), data.boundSources.end());
    }

    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    return sources;
}
