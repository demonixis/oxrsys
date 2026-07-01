// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <memory>
#include <unordered_map>

class Session;

class Instance
{
public:
    explicit Instance(XrVersion apiVersion, const std::vector<std::string>& enabledExtensions = {});
    ~Instance();

    uint64_t GetHandle() const
    {
        return handle_;
    }

    // System
    XrResult GetSystem(const XrSystemGetInfo* getInfo, XrSystemId* systemId);
    XrResult GetSystemProperties(XrSystemId systemId, XrSystemProperties* properties);
    XrResult GetInstanceProperties(XrInstanceProperties* properties);

    // View configuration
    XrResult EnumerateViewConfigurations(XrSystemId systemId, uint32_t viewConfigurationTypeCapacityInput,
                                         uint32_t* viewConfigurationTypeCountOutput,
                                         XrViewConfigurationType* viewConfigurationTypes);
    XrResult GetViewConfigurationProperties(XrSystemId systemId, XrViewConfigurationType viewConfigurationType,
                                             XrViewConfigurationProperties* configurationProperties);
    XrResult EnumerateViewConfigurationViews(XrSystemId systemId, XrViewConfigurationType viewConfigurationType,
                                              uint32_t viewCapacityInput, uint32_t* viewCountOutput,
                                              XrViewConfigurationView* views);
    XrResult EnumerateEnvironmentBlendModes(XrSystemId systemId, XrViewConfigurationType viewConfigurationType,
                                             uint32_t environmentBlendModeCapacityInput,
                                             uint32_t* environmentBlendModeCountOutput,
                                             XrEnvironmentBlendMode* environmentBlendModes);

    // Events
    void PushEvent(const XrEventDataBuffer& event);
    XrResult PollEvent(XrEventDataBuffer* eventData);
    void RemoveEventsForSession(XrSession session);

    // Session tracking
    Session* GetSession()
    {
        return session_;
    }
    void SetSession(Session* session)
    {
        session_ = session;
    }

    // Extensions
    bool IsExtensionEnabled(const char* extensionName) const;
    XrVersion GetApiVersion() const
    {
        return apiVersion_;
    }
    bool IsSystemIdValid(XrSystemId systemId) const;
    bool SupportsLocalFloor() const;
    bool SupportsPassthroughBlendMode() const { return passthroughBlendModeEnabled_; }
    void MarkMetalGraphicsRequirementsQueried();
    bool HasQueriedMetalGraphicsRequirements() const;
    void MarkVulkanGraphicsRequirementsQueried();
    bool HasQueriedVulkanGraphicsRequirements() const;
    void MarkOpenGLGraphicsRequirementsQueried();
    bool HasQueriedOpenGLGraphicsRequirements() const;
    void MarkD3D11GraphicsRequirementsQueried();
    bool HasQueriedD3D11GraphicsRequirements() const;
    void MarkD3D12GraphicsRequirementsQueried();
    bool HasQueriedD3D12GraphicsRequirements() const;
    bool IsViewConfigurationTypeSupported(XrViewConfigurationType viewConfigurationType) const;

    void SetDebugUtilsObjectName(XrObjectType objectType, uint64_t objectHandle, const char* objectName);
    std::string GetDebugUtilsObjectName(XrObjectType objectType, uint64_t objectHandle) const;

    static constexpr uint32_t EyeWidth = 1512;
    static constexpr uint32_t EyeHeight = 1680;

private:
    struct DebugUtilsObjectKey
    {
        XrObjectType objectType = XR_OBJECT_TYPE_UNKNOWN;
        uint64_t objectHandle = 0;

        bool operator==(const DebugUtilsObjectKey& other) const
        {
            return objectType == other.objectType && objectHandle == other.objectHandle;
        }
    };

    struct DebugUtilsObjectKeyHash
    {
        size_t operator()(const DebugUtilsObjectKey& key) const
        {
            return std::hash<int32_t>{}(static_cast<int32_t>(key.objectType)) ^
                   (std::hash<uint64_t>{}(key.objectHandle) << 1);
        }
    };

    uint64_t handle_ = 0;
    bool systemRequested_ = false;
    Session* session_ = nullptr;
    XrVersion apiVersion_ = XR_CURRENT_API_VERSION;
    std::vector<std::string> enabledExtensions_;
    bool passthroughBlendModeEnabled_ = false;

    std::mutex eventMutex_;
    std::deque<XrEventDataBuffer> eventQueue_;

    mutable std::mutex debugUtilsMutex_;
    std::unordered_map<DebugUtilsObjectKey, std::string, DebugUtilsObjectKeyHash> debugUtilsObjectNames_;
    bool metalGraphicsRequirementsQueried_ = false;
    bool vulkanGraphicsRequirementsQueried_ = false;
    bool openGLGraphicsRequirementsQueried_ = false;
    bool d3d11GraphicsRequirementsQueried_ = false;
    bool d3d12GraphicsRequirementsQueried_ = false;
};
