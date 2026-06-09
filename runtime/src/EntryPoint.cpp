// SPDX-License-Identifier: MPL-2.0

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_reflection.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

#ifdef XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#if defined(__APPLE__)
#include <vulkan/vulkan_metal.h>
#endif
#endif

#include <openxr/openxr_platform.h>

#include "Runtime.h"
#include "Instance.h"
#include "Session.h"
#include "Swapchain.h"
#include "Space.h"
#include "ActionSet.h"
#include "InputManager.h"
#include "HandTracker.h"
#include "Config.h"
#include "RuntimeStatus.h"
#include "VulkanDispatch.h"
#include "VulkanGraphicsContext.h"
#if defined(_WIN32)
#include "D3DGraphicsContext.h"
#endif

#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstring>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>

#if defined(_WIN32) && defined(XR_USE_GRAPHICS_API_D3D11)
#define OXRSYS_USE_D3D11 1
#endif
#if defined(_WIN32) && defined(XR_USE_GRAPHICS_API_D3D12)
#define OXRSYS_USE_D3D12 1
#endif

// We need to keep ownership of created objects
static std::unique_ptr<Instance> gInstance;
static std::unique_ptr<Session> gSession;
static std::vector<std::unique_ptr<ActionSetState>> gActionSets;
static std::vector<std::unique_ptr<ActionState>> gActions;

struct DebugUtilsMessengerState
{
    DebugUtilsMessengerState(Instance* instance, const XrDebugUtilsMessengerCreateInfoEXT& createInfo)
        : instance(instance),
          messageSeverities(createInfo.messageSeverities),
          messageTypes(createInfo.messageTypes),
          userCallback(createInfo.userCallback),
          userData(createInfo.userData)
    {
        Runtime::Get().RegisterHandle(handle, this);
    }

    ~DebugUtilsMessengerState()
    {
        Runtime::Get().RemoveHandle(handle);
    }

    uint64_t handle = 0;
    Instance* instance = nullptr;
    XrDebugUtilsMessageSeverityFlagsEXT messageSeverities = 0;
    XrDebugUtilsMessageTypeFlagsEXT messageTypes = 0;
    PFN_xrDebugUtilsMessengerCallbackEXT userCallback = nullptr;
    void* userData = nullptr;
};

// Metal device — retained from xrGetMetalGraphicsRequirementsKHR
static void* gMetalDevice = nullptr;

// Suggested bindings storage: interactionProfile path -> list of (action handle, binding path string)
struct SuggestedBinding
{
    uint64_t actionHandle;
    XrPath bindingPath = XR_NULL_PATH;
    XrPath topLevelPath = XR_NULL_PATH;
    std::string profilePathString;
    std::string bindingPathString; // e.g. "/user/hand/left/input/grip/pose"
    std::string componentPath;
};
static std::unordered_map<uint64_t, std::vector<SuggestedBinding>> gSuggestedBindings;
static bool gActionSetsAttached = false;
static std::vector<uint64_t> gAttachedActionSetHandles;
static std::vector<std::unique_ptr<DebugUtilsMessengerState>> gDebugUtilsMessengers;

// Hand trackers
static std::vector<std::unique_ptr<HandTracker>> gHandTrackers;

// Graphics API for the current session
#ifdef XR_USE_GRAPHICS_API_METAL
static GraphicsApi gGraphicsApi = GraphicsApi::Metal;
#else
static GraphicsApi gGraphicsApi = GraphicsApi::Vulkan;
#endif

#ifdef XR_USE_GRAPHICS_API_VULKAN
// Global Vulkan dispatch — definition (declared extern in VulkanDispatch.h)
VulkanDispatch gVulkanDispatch;

static bool EnsureVulkanInstanceDispatch(VkInstance vkInstance, const char* context)
{
    if (vkInstance == VK_NULL_HANDLE)
    {
        spdlog::error("OXRSys: {} requires a non-null Vulkan instance", context);
        return false;
    }

    if (gVulkanDispatch.enumeratePhysicalDevices && gVulkanDispatch.getDeviceProcAddr)
    {
        return true;
    }

    PFN_vkGetInstanceProcAddr getInstanceProcAddr =
        ResolveVulkanGetInstanceProcAddrFromProcess(gVulkanDispatch);
    if (getInstanceProcAddr == nullptr)
    {
        spdlog::error(
            "OXRSys: {} could not resolve vkGetInstanceProcAddr from the app-provided "
            "dispatch or from an already-loaded Vulkan loader",
            context);
        return false;
    }

    gVulkanDispatch.getInstanceProcAddr = getInstanceProcAddr;
    gVulkanDispatch.LoadInstanceFunctions(vkInstance);
    if (!gVulkanDispatch.enumeratePhysicalDevices || !gVulkanDispatch.getDeviceProcAddr)
    {
        spdlog::error("OXRSys: {} failed to resolve required Vulkan instance functions", context);
        return false;
    }

    return true;
}
#endif

#if defined(_WIN32)
static LUID GetDefaultAdapterLuid()
{
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory6));
    if (SUCCEEDED(result))
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT adapterIndex = 0;
             factory6->EnumAdapterByGpuPreference(
                 adapterIndex,
                 DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                 IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
             ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
                (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
            {
                return desc.AdapterLuid;
            }
            adapter.Reset();
        }
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory1;
    result = CreateDXGIFactory1(IID_PPV_ARGS(&factory1));
    if (SUCCEEDED(result))
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(factory1->EnumAdapters1(0, &adapter)))
        {
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(adapter->GetDesc1(&desc)))
            {
                return desc.AdapterLuid;
            }
        }
    }

    return {};
}

static LUID GetAdapterLuidForD3D11Device(ID3D11Device* device)
{
    if (device == nullptr)
    {
        return {};
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
    {
        return {};
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)))
    {
        return {};
    }
    DXGI_ADAPTER_DESC desc = {};
    if (FAILED(adapter->GetDesc(&desc)))
    {
        return {};
    }
    return desc.AdapterLuid;
}
#endif

static bool IsAttachedActionSetHandle(uint64_t actionSetHandle);

static void CleanupRuntimeState()
{
    gHandTrackers.clear();
    if (gSession)
    {
        gSession->Shutdown();
    }
    gSession.reset();
    gActions.clear();
    gActionSets.clear();
    gSuggestedBindings.clear();
    gActionSetsAttached = false;
    gAttachedActionSetHandles.clear();
    gDebugUtilsMessengers.clear();
    gInstance.reset();
}

// ============================================================================
// Helper to get instance from handle
// ============================================================================

static Instance* GetInstance(XrInstance instance)
{
    return Runtime::Get().FromHandle<Instance>(reinterpret_cast<uint64_t>(instance));
}

static Session* GetSession(XrSession session)
{
    return Runtime::Get().FromHandle<Session>(reinterpret_cast<uint64_t>(session));
}

static const char* GetResultName(XrResult value)
{
    switch (value)
    {
#define XR_RESULT_CASE(name, val) case name: return #name;
        XR_LIST_ENUM_XrResult(XR_RESULT_CASE)
#undef XR_RESULT_CASE
        default:
            return nullptr;
    }
}

static const char* GetStructureTypeName(XrStructureType value)
{
    switch (value)
    {
#define XR_STRUCTURE_CASE(name, val) case name: return #name;
        XR_LIST_ENUM_XrStructureType(XR_STRUCTURE_CASE)
#undef XR_STRUCTURE_CASE
        default:
            return nullptr;
    }
}

struct ExtensionInfo
{
    const char* name;
    uint32_t version;
};

#ifdef XR_USE_GRAPHICS_API_METAL
static constexpr const char* UNITY_METAL_ENABLE_EXTENSION_ALIAS = "XR_KHRX2_metal_enable";
static constexpr const char* UNITY_METAL_GRAPHICS_REQUIREMENTS_FUNCTION_ALIAS =
    "xrGetMetalGraphicsRequirementsKHRX2";
#endif
static constexpr const char* XR_LOCATE_SPACES_KHR_FUNCTION_ALIAS = "xrLocateSpacesKHR";

static const char* NormalizeExtensionName(const char* extensionName)
{
    if (extensionName == nullptr)
    {
        return nullptr;
    }

#ifdef XR_USE_GRAPHICS_API_METAL
    if (std::strcmp(extensionName, UNITY_METAL_ENABLE_EXTENSION_ALIAS) == 0)
    {
        return XR_KHR_METAL_ENABLE_EXTENSION_NAME;
    }
#endif

    return extensionName;
}

static std::vector<ExtensionInfo> GetSupportedExtensionInfos()
{
    std::vector<ExtensionInfo> extensions = {
#ifdef XR_USE_GRAPHICS_API_METAL
        {XR_KHR_METAL_ENABLE_EXTENSION_NAME, XR_KHR_metal_enable_SPEC_VERSION},
        {UNITY_METAL_ENABLE_EXTENSION_ALIAS, XR_KHR_metal_enable_SPEC_VERSION},
#endif
        {XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_EXT_hand_tracking_SPEC_VERSION},
        {XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME, XR_EXT_conformance_automation_SPEC_VERSION},
        {XR_EXT_HAND_INTERACTION_EXTENSION_NAME, XR_EXT_hand_interaction_SPEC_VERSION},
        {XR_EXT_DEBUG_UTILS_EXTENSION_NAME, XR_EXT_debug_utils_SPEC_VERSION},
#ifdef XR_META_touch_controller_plus
        {XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME, XR_META_touch_controller_plus_SPEC_VERSION},
#endif
    };
#ifdef XR_USE_GRAPHICS_API_VULKAN
    extensions.push_back({XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_vulkan_enable_SPEC_VERSION});
    extensions.push_back({XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_vulkan_enable2_SPEC_VERSION});
#endif
#ifdef OXRSYS_USE_D3D11
    extensions.push_back({XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_D3D11_enable_SPEC_VERSION});
#endif
#ifdef OXRSYS_USE_D3D12
    extensions.push_back({XR_KHR_D3D12_ENABLE_EXTENSION_NAME, XR_KHR_D3D12_enable_SPEC_VERSION});
#endif
    return extensions;
}

static std::vector<const char*> GetSupportedExtensions()
{
    std::vector<const char*> names;
    for (const auto& extension : GetSupportedExtensionInfos())
    {
        names.push_back(extension.name);
    }
    return names;
}

static bool IsSupportedExtensionName(const char* extensionName)
{
    extensionName = NormalizeExtensionName(extensionName);
    if (extensionName == nullptr)
    {
        return false;
    }

    std::vector<const char*> supported = GetSupportedExtensions();
    return std::any_of(supported.begin(), supported.end(),
                        [extensionName](const char* candidate)
                        {
                            return std::strcmp(candidate, extensionName) == 0;
                        });
}

static const char* ExtensionForFunctionName(const char* functionName)
{
    if (functionName == nullptr)
    {
        return nullptr;
    }

    if (std::strcmp(functionName, "xrCreateHandTrackerEXT") == 0 ||
        std::strcmp(functionName, "xrDestroyHandTrackerEXT") == 0 ||
        std::strcmp(functionName, "xrLocateHandJointsEXT") == 0)
    {
        return XR_EXT_HAND_TRACKING_EXTENSION_NAME;
    }
    if (std::strcmp(functionName, "xrSetInputDeviceActiveEXT") == 0 ||
        std::strcmp(functionName, "xrSetInputDeviceStateBoolEXT") == 0 ||
        std::strcmp(functionName, "xrSetInputDeviceStateFloatEXT") == 0 ||
        std::strcmp(functionName, "xrSetInputDeviceStateVector2fEXT") == 0 ||
        std::strcmp(functionName, "xrSetInputDeviceLocationEXT") == 0)
    {
        return XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME;
    }
    if (std::strcmp(functionName, "xrSetDebugUtilsObjectNameEXT") == 0 ||
        std::strcmp(functionName, "xrCreateDebugUtilsMessengerEXT") == 0 ||
        std::strcmp(functionName, "xrDestroyDebugUtilsMessengerEXT") == 0 ||
        std::strcmp(functionName, "xrSubmitDebugUtilsMessageEXT") == 0 ||
        std::strcmp(functionName, "xrSessionBeginDebugUtilsLabelRegionEXT") == 0 ||
        std::strcmp(functionName, "xrSessionEndDebugUtilsLabelRegionEXT") == 0 ||
        std::strcmp(functionName, "xrSessionInsertDebugUtilsLabelEXT") == 0)
    {
        return XR_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }
#ifdef XR_USE_GRAPHICS_API_METAL
    if (std::strcmp(functionName, "xrGetMetalGraphicsRequirementsKHR") == 0 ||
        std::strcmp(functionName, UNITY_METAL_GRAPHICS_REQUIREMENTS_FUNCTION_ALIAS) == 0)
    {
        return XR_KHR_METAL_ENABLE_EXTENSION_NAME;
    }
#endif
#ifdef XR_USE_GRAPHICS_API_VULKAN
    if (std::strcmp(functionName, "xrGetVulkanInstanceExtensionsKHR") == 0 ||
        std::strcmp(functionName, "xrGetVulkanDeviceExtensionsKHR") == 0 ||
        std::strcmp(functionName, "xrGetVulkanGraphicsDeviceKHR") == 0 ||
        std::strcmp(functionName, "xrGetVulkanGraphicsRequirementsKHR") == 0)
    {
        return XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
    }
    if (std::strcmp(functionName, "xrCreateVulkanInstanceKHR") == 0 ||
        std::strcmp(functionName, "xrCreateVulkanDeviceKHR") == 0 ||
        std::strcmp(functionName, "xrGetVulkanGraphicsDevice2KHR") == 0 ||
        std::strcmp(functionName, "xrGetVulkanGraphicsRequirements2KHR") == 0)
    {
        return XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
    }
#endif
#ifdef OXRSYS_USE_D3D11
    if (std::strcmp(functionName, "xrGetD3D11GraphicsRequirementsKHR") == 0)
    {
        return XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    }
#endif
#ifdef OXRSYS_USE_D3D12
    if (std::strcmp(functionName, "xrGetD3D12GraphicsRequirementsKHR") == 0)
    {
        return XR_KHR_D3D12_ENABLE_EXTENSION_NAME;
    }
#endif
    return nullptr;
}

static bool IsFunctionEnabledForInstance(Instance* instance, const char* functionName)
{
    if (instance != nullptr &&
        (std::strcmp(functionName, "xrLocateSpaces") == 0 ||
         std::strcmp(functionName, XR_LOCATE_SPACES_KHR_FUNCTION_ALIAS) == 0) &&
        instance->GetApiVersion() < XR_MAKE_VERSION(1, 1, 0))
    {
        return false;
    }

    const char* extensionName = ExtensionForFunctionName(functionName);
    if (extensionName == nullptr || instance == nullptr)
    {
        return true;
    }
    return instance->IsExtensionEnabled(extensionName);
}

static XrBaseOutStructure* FindNextStructure(void* next, XrStructureType type)
{
    auto* current = reinterpret_cast<XrBaseOutStructure*>(next);
    while (current != nullptr)
    {
        if (current->type == type)
        {
            return current;
        }
        current = current->next;
    }
    return nullptr;
}

static DebugUtilsMessengerState* CreateDebugUtilsMessengerState(
    Instance* instance, const XrDebugUtilsMessengerCreateInfoEXT* createInfo)
{
    if (instance == nullptr || createInfo == nullptr)
    {
        return nullptr;
    }

    auto messenger = std::make_unique<DebugUtilsMessengerState>(instance, *createInfo);
    DebugUtilsMessengerState* rawMessenger = messenger.get();
    gDebugUtilsMessengers.push_back(std::move(messenger));
    return rawMessenger;
}

static void DispatchDebugUtilsMessage(
    Instance* instance,
    XrDebugUtilsMessageSeverityFlagsEXT messageSeverity,
    XrDebugUtilsMessageTypeFlagsEXT messageTypes,
    const XrDebugUtilsMessengerCallbackDataEXT* callbackData)
{
    for (const auto& messenger : gDebugUtilsMessengers)
    {
        if (messenger->instance != instance || messenger->userCallback == nullptr)
        {
            continue;
        }
        if ((messenger->messageSeverities & messageSeverity) == 0 ||
            (messenger->messageTypes & messageTypes) == 0)
        {
            continue;
        }

        messenger->userCallback(messageSeverity, messageTypes, callbackData, messenger->userData);
    }
}

static void ResolveDebugUtilsObjects(
    Instance* instance,
    const XrDebugUtilsMessengerCallbackDataEXT* callbackData,
    std::vector<XrDebugUtilsObjectNameInfoEXT>& resolvedObjects,
    std::vector<std::string>& resolvedObjectNames)
{
    resolvedObjects.clear();
    resolvedObjectNames.clear();

    if (callbackData == nullptr || callbackData->objectCount == 0 || callbackData->objects == nullptr)
    {
        return;
    }

    resolvedObjects.assign(callbackData->objects, callbackData->objects + callbackData->objectCount);
    resolvedObjectNames.resize(callbackData->objectCount);

    for (uint32_t i = 0; i < callbackData->objectCount; ++i)
    {
        const XrDebugUtilsObjectNameInfoEXT& sourceObject = callbackData->objects[i];
        std::string resolvedName;
        if (instance != nullptr)
        {
            resolvedName = instance->GetDebugUtilsObjectName(sourceObject.objectType, sourceObject.objectHandle);
        }
        if (resolvedName.empty() && sourceObject.objectName != nullptr)
        {
            resolvedName = sourceObject.objectName;
        }

        resolvedObjectNames[i] = std::move(resolvedName);
    }

    for (size_t i = 0; i < resolvedObjects.size(); ++i)
    {
        resolvedObjects[i].objectName =
            resolvedObjectNames[i].empty() ? nullptr : resolvedObjectNames[i].c_str();
    }
}

static void ResolveDebugUtilsSessionLabels(
    const XrDebugUtilsMessengerCallbackDataEXT* callbackData,
    std::vector<XrDebugUtilsLabelEXT>& resolvedSessionLabels,
    std::vector<std::string>& resolvedSessionLabelNames)
{
    resolvedSessionLabels.clear();
    resolvedSessionLabelNames.clear();

    if (callbackData == nullptr || callbackData->objectCount == 0 || callbackData->objects == nullptr)
    {
        return;
    }

    std::vector<uint64_t> sessionHandles;
    for (uint32_t i = 0; i < callbackData->objectCount; ++i)
    {
        const XrDebugUtilsObjectNameInfoEXT& object = callbackData->objects[i];
        if (object.objectType != XR_OBJECT_TYPE_SESSION || object.objectHandle == 0)
        {
            continue;
        }

        if (std::find(sessionHandles.begin(), sessionHandles.end(), object.objectHandle) == sessionHandles.end())
        {
            sessionHandles.push_back(object.objectHandle);
        }
    }

    for (uint64_t sessionHandle : sessionHandles)
    {
        auto* session = GetSession(reinterpret_cast<XrSession>(sessionHandle));
        if (session == nullptr)
        {
            continue;
        }

        std::vector<XrDebugUtilsLabelEXT> sessionLabels;
        std::vector<std::string> sessionLabelNames;
        session->GetDebugUtilsLabels(sessionLabels, sessionLabelNames);

        for (size_t i = 0; i < sessionLabels.size(); ++i)
        {
            resolvedSessionLabelNames.push_back(sessionLabelNames[i]);
        }
    }

    resolvedSessionLabels.resize(resolvedSessionLabelNames.size(), {XR_TYPE_DEBUG_UTILS_LABEL_EXT});
    for (size_t i = 0; i < resolvedSessionLabels.size(); ++i)
    {
        resolvedSessionLabels[i].type = XR_TYPE_DEBUG_UTILS_LABEL_EXT;
        resolvedSessionLabels[i].next = nullptr;
        resolvedSessionLabels[i].labelName = resolvedSessionLabelNames[i].c_str();
    }
}

static void ResolveDebugUtilsCallbackData(
    Instance* instance,
    const XrDebugUtilsMessengerCallbackDataEXT* callbackData,
    XrDebugUtilsMessengerCallbackDataEXT& resolvedCallbackData,
    std::vector<XrDebugUtilsObjectNameInfoEXT>& resolvedObjects,
    std::vector<std::string>& resolvedObjectNames,
    std::vector<XrDebugUtilsLabelEXT>& resolvedSessionLabels,
    std::vector<std::string>& resolvedSessionLabelNames)
{
    resolvedCallbackData = *callbackData;

    ResolveDebugUtilsObjects(instance, callbackData, resolvedObjects, resolvedObjectNames);
    ResolveDebugUtilsSessionLabels(callbackData, resolvedSessionLabels, resolvedSessionLabelNames);

    resolvedCallbackData.objectCount = static_cast<uint32_t>(resolvedObjects.size());
    resolvedCallbackData.objects = resolvedObjects.empty() ? nullptr : resolvedObjects.data();
    resolvedCallbackData.sessionLabelCount = static_cast<uint32_t>(resolvedSessionLabels.size());
    resolvedCallbackData.sessionLabels = resolvedSessionLabels.empty() ? nullptr : resolvedSessionLabels.data();
}

// ============================================================================
// Global functions (instance == NULL is valid)
// ============================================================================

static XRAPI_ATTR XrResult XRAPI_CALL OxrEnumerateInstanceExtensionProperties(
    const char* /*layerName*/, uint32_t propertyCapacityInput,
    uint32_t* propertyCountOutput, XrExtensionProperties* properties)
{
    const std::vector<ExtensionInfo> extensions = GetSupportedExtensionInfos();
    const uint32_t extCount = static_cast<uint32_t>(extensions.size());

    if (propertyCountOutput == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *propertyCountOutput = extCount;
    if (propertyCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (propertyCapacityInput < extCount)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    for (uint32_t i = 0; i < extCount; i++)
    {
        properties[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        properties[i].next = nullptr;
        std::strncpy(properties[i].extensionName, extensions[i].name, XR_MAX_EXTENSION_NAME_SIZE);
        properties[i].extensionVersion = extensions[i].version;
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrEnumerateApiLayerProperties(
    uint32_t /*propertyCapacityInput*/, uint32_t* propertyCountOutput,
    XrApiLayerProperties* /*properties*/)
{
    if (propertyCountOutput == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *propertyCountOutput = 0;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrCreateInstance(
    const XrInstanceCreateInfo* createInfo, XrInstance* instance)
{
    if (createInfo == nullptr || instance == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (gInstance)
    {
        return XR_ERROR_LIMIT_REACHED;
    }
    if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->createFlags != 0)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->applicationInfo.applicationName[0] == '\0')
    {
        return XR_ERROR_NAME_INVALID;
    }
    if (std::memchr(createInfo->applicationInfo.applicationName, '\0', XR_MAX_APPLICATION_NAME_SIZE) == nullptr)
    {
        return XR_ERROR_NAME_INVALID;
    }
    if (std::memchr(createInfo->applicationInfo.engineName, '\0', XR_MAX_ENGINE_NAME_SIZE) == nullptr)
    {
        return XR_ERROR_NAME_INVALID;
    }

    const XrVersion requestedApiVersion = createInfo->applicationInfo.apiVersion;
    if (requestedApiVersion == 0 || XR_VERSION_MAJOR(requestedApiVersion) != XR_VERSION_MAJOR(XR_CURRENT_API_VERSION) ||
        requestedApiVersion > XR_CURRENT_API_VERSION)
    {
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    }

    const ConfigValues config = Config::Get().GetValues();
    if (!config.runtimeEnabled)
    {
        spdlog::info("OXRSys: Rejecting xrCreateInstance because runtime_enabled=false");
        return XR_ERROR_RUNTIME_UNAVAILABLE;
    }

    spdlog::info("OXRSys: Creating instance for '{}'", createInfo->applicationInfo.applicationName);

    std::vector<std::string> enabledExtensions;
    enabledExtensions.reserve(createInfo->enabledExtensionCount);

    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++)
    {
        const char* extensionName = createInfo->enabledExtensionNames[i];
        if (!IsSupportedExtensionName(extensionName))
        {
            spdlog::warn("OXRSys: Unsupported extension requested: {}", extensionName);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
        enabledExtensions.emplace_back(NormalizeExtensionName(extensionName));
    }

    gInstance = std::make_unique<Instance>(createInfo->applicationInfo.apiVersion, enabledExtensions);
    RuntimeStatus::SetApplicationName(createInfo->applicationInfo.applicationName);
    *instance = reinterpret_cast<XrInstance>(gInstance->GetHandle());
    return XR_SUCCESS;
}

// ============================================================================
// Instance functions
// ============================================================================

static XRAPI_ATTR XrResult XRAPI_CALL OxrDestroyInstance(XrInstance instance)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    CleanupRuntimeState();
    RuntimeStatus::ClearApplicationName();
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetInstanceProperties(
    XrInstance instance, XrInstanceProperties* instanceProperties)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return inst->GetInstanceProperties(instanceProperties);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetSystem(
    XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return inst->GetSystem(getInfo, systemId);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetSystemProperties(
    XrInstance instance, XrSystemId systemId, XrSystemProperties* properties)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return inst->GetSystemProperties(systemId, properties);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrPollEvent(
    XrInstance instance, XrEventDataBuffer* eventData)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return inst->PollEvent(eventData);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrResultToString(
    XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE])
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (buffer == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    const char* resultName = GetResultName(value);
    if (resultName)
    {
        std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s", resultName);
        return XR_SUCCESS;
    }

    if (static_cast<int32_t>(value) < 0)
    {
        std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "XR_UNKNOWN_FAILURE_%d", static_cast<int32_t>(value));
    }
    else
    {
        std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "XR_UNKNOWN_SUCCESS_%d", static_cast<int32_t>(value));
    }

    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrStructureTypeToString(
    XrInstance instance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE])
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (buffer == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    const char* structureTypeName = GetStructureTypeName(value);
    if (structureTypeName)
    {
        std::snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "%s", structureTypeName);
        return XR_SUCCESS;
    }

    std::snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d", static_cast<int32_t>(value));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrStringToPath(
    XrInstance instance, const char* pathString, XrPath* path)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return Runtime::Get().StringToPath(pathString, path);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrPathToString(
    XrInstance instance, XrPath path,
    uint32_t bufferCapacityInput, uint32_t* bufferCountOutput, char* buffer)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return Runtime::Get().PathToString(path, bufferCapacityInput, bufferCountOutput, buffer);
}

// ============================================================================
// View configuration
// ============================================================================

static XRAPI_ATTR XrResult XRAPI_CALL OxrEnumerateViewConfigurations(
    XrInstance instance, XrSystemId systemId,
    uint32_t viewConfigurationTypeCapacityInput, uint32_t* viewConfigurationTypeCountOutput,
    XrViewConfigurationType* viewConfigurationTypes)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return inst->EnumerateViewConfigurations(systemId, viewConfigurationTypeCapacityInput,
                                              viewConfigurationTypeCountOutput, viewConfigurationTypes);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetViewConfigurationProperties(
    XrInstance instance, XrSystemId systemId, XrViewConfigurationType viewConfigurationType,
    XrViewConfigurationProperties* configurationProperties)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return inst->GetViewConfigurationProperties(systemId, viewConfigurationType, configurationProperties);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrEnumerateViewConfigurationViews(
    XrInstance instance, XrSystemId systemId, XrViewConfigurationType viewConfigurationType,
    uint32_t viewCapacityInput, uint32_t* viewCountOutput, XrViewConfigurationView* views)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return inst->EnumerateViewConfigurationViews(systemId, viewConfigurationType,
                                                  viewCapacityInput, viewCountOutput, views);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrEnumerateEnvironmentBlendModes(
    XrInstance instance, XrSystemId systemId, XrViewConfigurationType viewConfigurationType,
    uint32_t environmentBlendModeCapacityInput, uint32_t* environmentBlendModeCountOutput,
    XrEnvironmentBlendMode* environmentBlendModes)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return inst->EnumerateEnvironmentBlendModes(systemId, viewConfigurationType,
                                                 environmentBlendModeCapacityInput,
                                                 environmentBlendModeCountOutput, environmentBlendModes);
}

// ============================================================================
// Session
// ============================================================================

static XRAPI_ATTR XrResult XRAPI_CALL OxrCreateSession(
    XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session)
{
    auto* inst = GetInstance(instance);
    if (!inst || createInfo == nullptr || session == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (!inst->IsSystemIdValid(createInfo->systemId))
    {
        return XR_ERROR_SYSTEM_INVALID;
    }

    // Extract graphics binding from the next chain
#ifdef XR_USE_GRAPHICS_API_METAL
    const XrGraphicsBindingMetalKHR* metalBinding = nullptr;
#endif
#ifdef XR_USE_GRAPHICS_API_VULKAN
    const XrGraphicsBindingVulkanKHR* vulkanBinding = nullptr;
#endif
#ifdef OXRSYS_USE_D3D11
    const XrGraphicsBindingD3D11KHR* d3d11Binding = nullptr;
#endif
#ifdef OXRSYS_USE_D3D12
    const XrGraphicsBindingD3D12KHR* d3d12Binding = nullptr;
#endif
    const XrBaseInStructure* next = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
    while (next)
    {
#ifdef XR_USE_GRAPHICS_API_METAL
        if (next->type == XR_TYPE_GRAPHICS_BINDING_METAL_KHR)
        {
            if (!inst->IsExtensionEnabled(XR_KHR_METAL_ENABLE_EXTENSION_NAME))
            {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            metalBinding = reinterpret_cast<const XrGraphicsBindingMetalKHR*>(next);
            if (metalBinding->commandQueue == nullptr)
            {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            if (!inst->HasQueriedMetalGraphicsRequirements())
            {
                return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
            }
        }
#endif
#ifdef XR_USE_GRAPHICS_API_VULKAN
        if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR)
        {
            if (!inst->IsExtensionEnabled(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) &&
                !inst->IsExtensionEnabled(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
            {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            vulkanBinding = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(next);
            if (!inst->HasQueriedVulkanGraphicsRequirements())
            {
                return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
            }
        }
#endif
#ifdef OXRSYS_USE_D3D11
        if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
        {
            if (!inst->IsExtensionEnabled(XR_KHR_D3D11_ENABLE_EXTENSION_NAME))
            {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            d3d11Binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
            if (d3d11Binding->device == nullptr)
            {
                return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            }
            if (!inst->HasQueriedD3D11GraphicsRequirements())
            {
                return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
            }
        }
#endif
#ifdef OXRSYS_USE_D3D12
        if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR)
        {
            if (!inst->IsExtensionEnabled(XR_KHR_D3D12_ENABLE_EXTENSION_NAME))
            {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            d3d12Binding = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(next);
            if (d3d12Binding->device == nullptr || d3d12Binding->queue == nullptr)
            {
                return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            }
            if (!inst->HasQueriedD3D12GraphicsRequirements())
            {
                return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
            }
        }
#endif
        next = next->next;
    }

    if (gSession)
    {
        return XR_ERROR_LIMIT_REACHED;
    }

#ifdef XR_USE_GRAPHICS_API_METAL
    // Ensure we have a Metal device for the debug Renderer
    if (!gMetalDevice)
    {
        typedef void* (*MTLCreateSystemDefaultDeviceFn)(void);
        void* metalFramework = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_LAZY);
        if (metalFramework)
        {
            auto createDevice = (MTLCreateSystemDefaultDeviceFn)dlsym(metalFramework,
                                                                        "MTLCreateSystemDefaultDevice");
            if (createDevice)
            {
                gMetalDevice = createDevice();
            }
        }
    }
#endif

#ifdef XR_USE_GRAPHICS_API_VULKAN
    if (vulkanBinding)
    {
        if (!EnsureVulkanInstanceDispatch(vulkanBinding->instance, "xrCreateSession Vulkan binding"))
        {
            return XR_ERROR_RUNTIME_FAILURE;
        }

        VulkanGraphicsContext vulkanContext = {};
        vulkanContext.instance = vulkanBinding->instance;
        vulkanContext.physicalDevice = vulkanBinding->physicalDevice;
        vulkanContext.device = vulkanBinding->device;
        vulkanContext.queueFamilyIndex = vulkanBinding->queueFamilyIndex;
        vulkanContext.queueIndex = vulkanBinding->queueIndex;

        PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
        if (gVulkanDispatch.getDeviceProcAddr != nullptr)
        {
            getDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
                gVulkanDispatch.getDeviceProcAddr(vulkanContext.device, "vkGetDeviceQueue"));
        }
        if (getDeviceQueue == nullptr)
        {
            getDeviceQueue = gVulkanDispatch.getDeviceQueue;
        }
        if (getDeviceQueue != nullptr)
        {
            getDeviceQueue(vulkanContext.device,
                           vulkanContext.queueFamilyIndex,
                           vulkanContext.queueIndex,
                           &vulkanContext.queue);
        }
        if (vulkanContext.queue == VK_NULL_HANDLE)
        {
            spdlog::error("OXRSys: Vulkan graphics binding did not provide a resolvable queue");
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }

        gGraphicsApi = GraphicsApi::Vulkan;
        gSession = std::make_unique<Session>(
            inst, GraphicsContext::Vulkan(vulkanContext, gMetalDevice));
        *session = reinterpret_cast<XrSession>(gSession->GetHandle());
        spdlog::info("OXRSys: Session created with Vulkan binding");
        return XR_SUCCESS;
    }
#endif

#ifdef OXRSYS_USE_D3D11
    if (d3d11Binding)
    {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediateContext;
        d3d11Binding->device->GetImmediateContext(&immediateContext);
        if (immediateContext == nullptr)
        {
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }

        D3D11GraphicsContext d3d11Context = {};
        d3d11Context.device = d3d11Binding->device;
        d3d11Context.immediateContext = immediateContext.Get();
        d3d11Context.adapterLuid = GetAdapterLuidForD3D11Device(d3d11Binding->device);

        gGraphicsApi = GraphicsApi::D3D11;
        gSession = std::make_unique<Session>(inst, d3d11Context);
        *session = reinterpret_cast<XrSession>(gSession->GetHandle());
        spdlog::info("OXRSys: Session created with D3D11 binding");
        return XR_SUCCESS;
    }
#endif

#ifdef OXRSYS_USE_D3D12
    if (d3d12Binding)
    {
        D3D12GraphicsContext d3d12Context = {};
        d3d12Context.device = d3d12Binding->device;
        d3d12Context.queue = d3d12Binding->queue;
        d3d12Context.adapterLuid = d3d12Binding->device->GetAdapterLuid();

        gGraphicsApi = GraphicsApi::D3D12;
        gSession = std::make_unique<Session>(inst, d3d12Context);
        *session = reinterpret_cast<XrSession>(gSession->GetHandle());
        spdlog::info("OXRSys: Session created with D3D12 binding");
        return XR_SUCCESS;
    }
#endif

#ifdef XR_USE_GRAPHICS_API_METAL
    if (metalBinding)
    {
        gGraphicsApi = GraphicsApi::Metal;
        gSession = std::make_unique<Session>(inst, gMetalDevice, metalBinding->commandQueue);
        *session = reinterpret_cast<XrSession>(gSession->GetHandle());
        spdlog::info("OXRSys: Session created with Metal binding");
        return XR_SUCCESS;
    }
#endif

    spdlog::error("OXRSys: No supported graphics binding provided");
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrDestroySession(XrSession session)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    sess->Shutdown();
    gHandTrackers.clear();
    gActionSetsAttached = false;
    gAttachedActionSetHandles.clear();
    gSession.reset();
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrBeginSession(
    XrSession session, const XrSessionBeginInfo* beginInfo)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sess->BeginSession(beginInfo);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrEndSession(XrSession session)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sess->EndSession();
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrRequestExitSession(XrSession session)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sess->RequestExitSession();
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrWaitFrame(
    XrSession session, const XrFrameWaitInfo* frameWaitInfo, XrFrameState* frameState)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sess->WaitFrame(frameWaitInfo, frameState);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrBeginFrame(
    XrSession session, const XrFrameBeginInfo* frameBeginInfo)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sess->BeginFrame(frameBeginInfo);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrEndFrame(
    XrSession session, const XrFrameEndInfo* frameEndInfo)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sess->EndFrame(frameEndInfo);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrLocateViews(
    XrSession session, const XrViewLocateInfo* viewLocateInfo,
    XrViewState* viewState, uint32_t viewCapacityInput,
    uint32_t* viewCountOutput, XrView* views)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sess->LocateViews(viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
}

// ============================================================================
// Swapchain
// ============================================================================

static XRAPI_ATTR XrResult XRAPI_CALL OxrEnumerateSwapchainFormats(
    XrSession session, uint32_t formatCapacityInput,
    uint32_t* formatCountOutput, int64_t* formats)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    // Metal pixel formats
    static const int64_t metalFormats[] = {
        81,  // MTLPixelFormatBGRA8Unorm_sRGB
        80,  // MTLPixelFormatBGRA8Unorm
        71,  // MTLPixelFormatRGBA8Unorm_sRGB
        70,  // MTLPixelFormatRGBA8Unorm
        252, // MTLPixelFormatDepth32Float
        260, // MTLPixelFormatDepth32Float_Stencil8
    };

#ifdef XR_USE_GRAPHICS_API_VULKAN
    // Vulkan formats (VkFormat values)
    static const int64_t vulkanFormats[] = {
        50,  // VK_FORMAT_B8G8R8A8_SRGB
        44,  // VK_FORMAT_B8G8R8A8_UNORM
        43,  // VK_FORMAT_R8G8B8A8_SRGB
        37,  // VK_FORMAT_R8G8B8A8_UNORM
        126, // VK_FORMAT_D32_SFLOAT
        130, // VK_FORMAT_D32_SFLOAT_S8_UINT
    };
#endif
#if defined(OXRSYS_USE_D3D11) || defined(OXRSYS_USE_D3D12)
    static const int64_t d3dFormats[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_D16_UNORM,
        DXGI_FORMAT_D24_UNORM_S8_UINT,
        DXGI_FORMAT_D32_FLOAT,
        DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
    };
#endif

    const int64_t* supportedFormats = metalFormats;
    uint32_t formatCount = sizeof(metalFormats) / sizeof(metalFormats[0]);

#ifdef XR_USE_GRAPHICS_API_VULKAN
    if (gGraphicsApi == GraphicsApi::Vulkan)
    {
        supportedFormats = vulkanFormats;
        formatCount = sizeof(vulkanFormats) / sizeof(vulkanFormats[0]);
    }
#endif
#if defined(OXRSYS_USE_D3D11) || defined(OXRSYS_USE_D3D12)
    if (gGraphicsApi == GraphicsApi::D3D11 || gGraphicsApi == GraphicsApi::D3D12)
    {
        supportedFormats = d3dFormats;
        formatCount = sizeof(d3dFormats) / sizeof(d3dFormats[0]);
    }
#endif

    if (formatCountOutput == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *formatCountOutput = formatCount;
    if (formatCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (formatCapacityInput < formatCount)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    std::memcpy(formats, supportedFormats, formatCount * sizeof(int64_t));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrCreateSwapchain(
    XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sess->CreateSwapchain(createInfo, swapchain);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrDestroySwapchain(XrSwapchain swapchain)
{
    auto* sc = Runtime::Get().FromHandle<Swapchain>(reinterpret_cast<uint64_t>(swapchain));
    if (!sc)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    // Find the session that owns this swapchain
    if (gSession)
    {
        return gSession->DestroySwapchain(sc);
    }
    return XR_ERROR_HANDLE_INVALID;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrEnumerateSwapchainImages(
    XrSwapchain swapchain, uint32_t imageCapacityInput,
    uint32_t* imageCountOutput, XrSwapchainImageBaseHeader* images)
{
    auto* sc = Runtime::Get().FromHandle<Swapchain>(reinterpret_cast<uint64_t>(swapchain));
    if (!sc)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sc->EnumerateImages(imageCapacityInput, imageCountOutput, images);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrAcquireSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index)
{
    auto* sc = Runtime::Get().FromHandle<Swapchain>(reinterpret_cast<uint64_t>(swapchain));
    if (!sc)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sc->AcquireImage(acquireInfo, index);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrWaitSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo)
{
    auto* sc = Runtime::Get().FromHandle<Swapchain>(reinterpret_cast<uint64_t>(swapchain));
    if (!sc)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sc->WaitImage(waitInfo);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrReleaseSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo)
{
    auto* sc = Runtime::Get().FromHandle<Swapchain>(reinterpret_cast<uint64_t>(swapchain));
    if (!sc)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sc->ReleaseImage(releaseInfo);
}

// ============================================================================
// Spaces
// ============================================================================

static XRAPI_ATTR XrResult XRAPI_CALL OxrEnumerateReferenceSpaces(
    XrSession session, uint32_t spaceCapacityInput,
    uint32_t* spaceCountOutput, XrReferenceSpaceType* spaces)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::vector<XrReferenceSpaceType> supportedSpaces = {
        XR_REFERENCE_SPACE_TYPE_VIEW,
        XR_REFERENCE_SPACE_TYPE_LOCAL,
        XR_REFERENCE_SPACE_TYPE_STAGE,
    };

    if (sess->GetInstance()->SupportsLocalFloor())
    {
        supportedSpaces.insert(supportedSpaces.begin() + 2, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR);
    }

    if (spaceCountOutput == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *spaceCountOutput = static_cast<uint32_t>(supportedSpaces.size());
    if (spaceCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (spaceCapacityInput < *spaceCountOutput)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    std::memcpy(spaces, supportedSpaces.data(), supportedSpaces.size() * sizeof(XrReferenceSpaceType));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrCreateReferenceSpace(
    XrSession session, const XrReferenceSpaceCreateInfo* createInfo, XrSpace* space)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sess->CreateReferenceSpace(createInfo, space);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrDestroySpace(XrSpace space)
{
    auto* sp = Runtime::Get().FromHandle<Space>(reinterpret_cast<uint64_t>(space));
    if (!sp)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (gSession)
    {
        return gSession->DestroySpace(sp);
    }
    return XR_ERROR_HANDLE_INVALID;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrLocateSpace(
    XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location)
{
    auto* sp = Runtime::Get().FromHandle<Space>(reinterpret_cast<uint64_t>(space));
    auto* base = Runtime::Get().FromHandle<Space>(reinterpret_cast<uint64_t>(baseSpace));
    if (!sp || !base)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    return sp->LocateSpace(base, time, location);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrLocateSpaces(
    XrSession session, const XrSpacesLocateInfo* locateInfo, XrSpaceLocations* spaceLocations)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (locateInfo == nullptr || spaceLocations == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    auto* baseSpace = Runtime::Get().FromHandle<Space>(reinterpret_cast<uint64_t>(locateInfo->baseSpace));
    if (baseSpace == nullptr || baseSpace->GetSession() != sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    const uint32_t requiredLocationCount = locateInfo->spaceCount;
    if (requiredLocationCount == 0 || spaceLocations->locationCount != requiredLocationCount)
    {
        spaceLocations->locationCount = requiredLocationCount;
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (requiredLocationCount > 0 && locateInfo->spaces == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    uint32_t locationCapacity = spaceLocations->locationCount;
    spaceLocations->locationCount = requiredLocationCount;
    if (requiredLocationCount > 0 &&
        (locationCapacity < requiredLocationCount || spaceLocations->locations == nullptr))
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    auto* velocities = reinterpret_cast<XrSpaceVelocities*>(
        FindNextStructure(spaceLocations->next, XR_TYPE_SPACE_VELOCITIES));
    if (velocities != nullptr)
    {
        uint32_t velocityCapacity = velocities->velocityCount;
        velocities->velocityCount = requiredLocationCount;
        if (requiredLocationCount > 0 && velocities->velocities == nullptr)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        if (velocityCapacity != requiredLocationCount)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        if (requiredLocationCount > 0 && velocityCapacity < requiredLocationCount)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        for (uint32_t i = 0; i < requiredLocationCount; ++i)
        {
            velocities->velocities[i].velocityFlags = 0;
            velocities->velocities[i].linearVelocity = {0.0f, 0.0f, 0.0f};
            velocities->velocities[i].angularVelocity = {0.0f, 0.0f, 0.0f};
        }
    }

    for (uint32_t i = 0; i < requiredLocationCount; ++i)
    {
        auto* space = Runtime::Get().FromHandle<Space>(reinterpret_cast<uint64_t>(locateInfo->spaces[i]));
        if (space == nullptr || space->GetSession() != sess)
        {
            return XR_ERROR_HANDLE_INVALID;
        }

        XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
        XrResult result = space->LocateSpace(baseSpace, locateInfo->time, &location);
        if (XR_FAILED(result))
        {
            return result;
        }

        spaceLocations->locations[i].locationFlags = location.locationFlags;
        spaceLocations->locations[i].pose = location.pose;
        if (velocities != nullptr &&
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0)
        {
            velocities->velocities[i].velocityFlags =
                XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
        }
    }

    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetReferenceSpaceBoundsRect(
    XrSession session, XrReferenceSpaceType referenceSpaceType, XrExtent2Df* bounds)
{
    auto* sess = GetSession(session);
    if (sess == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (bounds == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    switch (referenceSpaceType)
    {
        case XR_REFERENCE_SPACE_TYPE_STAGE:
            bounds->width = 5.0f;
            bounds->height = 5.0f;
            return XR_SUCCESS;

        case XR_REFERENCE_SPACE_TYPE_VIEW:
        case XR_REFERENCE_SPACE_TYPE_LOCAL:
        case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
            bounds->width = 0.0f;
            bounds->height = 0.0f;
            return referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR &&
                           !sess->GetInstance()->SupportsLocalFloor()
                       ? XR_ERROR_REFERENCE_SPACE_UNSUPPORTED
                       : XR_SPACE_BOUNDS_UNAVAILABLE;

        default:
            bounds->width = 0.0f;
            bounds->height = 0.0f;
            return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }
}

// ============================================================================
// Actions
// ============================================================================

static std::string ComponentFromBindingPath(const std::string& path);

static bool IsValidSingleLevelPathName(const char* name)
{
    if (name == nullptr || name[0] == '\0')
    {
        return false;
    }

    for (const unsigned char ch : std::string(name))
    {
        if (std::islower(ch) || std::isdigit(ch) || ch == '_' || ch == '-' || ch == '.')
        {
            continue;
        }
        return false;
    }

    return true;
}

static bool IsSupportedActionSubactionPath(const std::string& path)
{
    return path == "/user/hand/left" || path == "/user/hand/right" || path == "/user/gamepad";
}

static bool IsKnownInteractionProfilePath(const Instance* instance, const std::string& profilePath)
{
    static const std::unordered_set<std::string> profiles10 = {
        "/interaction_profiles/khr/simple_controller",
        "/interaction_profiles/google/daydream_controller",
        "/interaction_profiles/htc/vive_controller",
        "/interaction_profiles/htc/vive_pro",
        "/interaction_profiles/microsoft/motion_controller",
        "/interaction_profiles/microsoft/xbox_controller",
        "/interaction_profiles/oculus/go_controller",
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/valve/index_controller",
    };

    static const std::unordered_set<std::string> profiles11 = {
        "/interaction_profiles/bytedance/pico_neo3_controller",
        "/interaction_profiles/bytedance/pico4_controller",
        "/interaction_profiles/bytedance/pico_g3_controller",
        "/interaction_profiles/hp/mixed_reality_controller",
        "/interaction_profiles/htc/vive_cosmos_controller",
        "/interaction_profiles/htc/vive_focus3_controller",
        "/interaction_profiles/meta/touch_controller_rift_cv1",
        "/interaction_profiles/meta/touch_controller_quest_1_rift_s",
        "/interaction_profiles/meta/touch_controller_quest_2",
        "/interaction_profiles/meta/touch_plus_controller",
        "/interaction_profiles/meta/touch_pro_controller",
        "/interaction_profiles/ml/ml2_controller",
        "/interaction_profiles/samsung/odyssey_controller",
    };

    if (profiles10.contains(profilePath))
    {
        return true;
    }

    if (profilePath == "/interaction_profiles/ext/hand_interaction_ext")
    {
        return instance->IsExtensionEnabled(XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
    }
#ifdef XR_META_touch_controller_plus
    if (profilePath == "/interaction_profiles/meta/touch_controller_plus")
    {
        return instance->IsExtensionEnabled(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
    }
#endif

    return instance->GetApiVersion() >= XR_MAKE_VERSION(1, 1, 0) && profiles11.contains(profilePath);
}

static bool IsValidBindingPathForProfile(const Instance* instance,
                                         const std::string& profilePath,
                                         const std::string& bindingPath)
{
    auto parseComponent = [](const std::string& path, std::string* topLevelPath, std::string* componentPath) {
        constexpr const char* inputMarker = "/input/";
        constexpr const char* outputMarker = "/output/";

        size_t inputPos = path.find(inputMarker);
        size_t outputPos = path.find(outputMarker);
        size_t markerPos = std::min(inputPos == std::string::npos ? path.size() : inputPos,
                                    outputPos == std::string::npos ? path.size() : outputPos);

        if (markerPos == path.size())
        {
            return false;
        }

        if (topLevelPath != nullptr)
        {
            *topLevelPath = path.substr(0, markerPos);
        }

        if (inputPos != std::string::npos && inputPos == markerPos)
        {
            if (componentPath != nullptr)
            {
                *componentPath = path.substr(inputPos + std::strlen(inputMarker));
            }
            return true;
        }

        if (outputPos != std::string::npos && outputPos == markerPos)
        {
            if (componentPath != nullptr)
            {
                *componentPath = path.substr(outputPos + std::strlen(outputMarker));
            }
            return true;
        }

        return false;
    };

    std::string topLevelPath;
    std::string componentPath;
    if (!parseComponent(bindingPath, &topLevelPath, &componentPath) || componentPath.empty())
    {
        return false;
    }

    auto countSegments = [](const std::string& path) {
        return static_cast<int>(std::count(path.begin(), path.end(), '/') + 1);
    };

    if (topLevelPath != "/user/hand/left" && topLevelPath != "/user/hand/right" &&
        topLevelPath != "/user/gamepad" && topLevelPath != "/user/head")
    {
        return false;
    }

    if (componentPath == "haptic")
    {
        return bindingPath == topLevelPath + "/output/haptic";
    }

    if (componentPath == "palm_ext/pose")
    {
        return false;
    }
    if ((componentPath == "trigger/proximity" || componentPath == "thumb_resting_surfaces/proximity") &&
        instance->GetApiVersion() < XR_MAKE_VERSION(1, 1, 0))
    {
        return false;
    }
    if (componentPath.ends_with("/proximity_fb"))
    {
        return false;
    }
    if (componentPath.find("/dpad_") != std::string::npos)
    {
        return false;
    }
    if (componentPath == "pinch_ext/pose" || componentPath == "pinch_ext/value" ||
        componentPath == "pinch_ext/ready_ext" || componentPath == "poke_ext/pose" ||
        componentPath == "grasp_ext/ready_ext" || componentPath == "grasp_ext/value" ||
        componentPath == "aim_activate_ext/ready_ext" || componentPath == "aim_activate_ext/value")
    {
        return (topLevelPath == "/user/hand/left" || topLevelPath == "/user/hand/right") &&
               instance->IsExtensionEnabled(XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
    }

    if (countSegments(componentPath) > 2)
    {
        return false;
    }

    if (profilePath == "/interaction_profiles/ext/hand_interaction_ext")
    {
        static const std::unordered_set<std::string> supportedComponents = {
            "aim/pose",
            "grip/pose",
            "grip_surface/pose",
            "pinch_ext/pose",
            "pinch_ext/value",
            "pinch_ext/ready_ext",
            "poke_ext/pose",
            "grasp_ext/ready_ext",
            "grasp_ext/value",
            "aim_activate_ext/ready_ext",
            "aim_activate_ext/value",
        };
        if (componentPath == "grip_surface/pose")
        {
            return (topLevelPath == "/user/hand/left" || topLevelPath == "/user/hand/right") &&
                   instance->GetApiVersion() >= XR_MAKE_VERSION(1, 1, 0);
        }
        return (topLevelPath == "/user/hand/left" || topLevelPath == "/user/hand/right") &&
               supportedComponents.contains(componentPath);
    }

    if (componentPath == "grip_surface/pose" && instance->GetApiVersion() < XR_MAKE_VERSION(1, 1, 0))
    {
        return false;
    }

    return true;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrCreateActionSet(
    XrInstance instance, const XrActionSetCreateInfo* createInfo, XrActionSet* actionSet)
{
    auto* inst = GetInstance(instance);
    if (inst == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || actionSet == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->type != XR_TYPE_ACTION_SET_CREATE_INFO)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->actionSetName[0] == '\0')
    {
        return XR_ERROR_NAME_INVALID;
    }
    if (createInfo->localizedActionSetName[0] == '\0')
    {
        return XR_ERROR_LOCALIZED_NAME_INVALID;
    }
    if (!IsValidSingleLevelPathName(createInfo->actionSetName))
    {
        return XR_ERROR_PATH_FORMAT_INVALID;
    }
    for (const auto& existingActionSet : gActionSets)
    {
        if (existingActionSet->GetName() == createInfo->actionSetName)
        {
            return XR_ERROR_NAME_DUPLICATED;
        }
        if (existingActionSet->GetLocalizedName() == createInfo->localizedActionSetName)
        {
            return XR_ERROR_LOCALIZED_NAME_DUPLICATED;
        }
    }

    auto as = std::make_unique<ActionSetState>(createInfo);
    *actionSet = reinterpret_cast<XrActionSet>(as->GetHandle());
    gActionSets.push_back(std::move(as));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrDestroyActionSet(XrActionSet actionSet)
{
    uint64_t handle = reinterpret_cast<uint64_t>(actionSet);
    for (auto it = gActionSets.begin(); it != gActionSets.end(); ++it)
    {
        if ((*it)->GetHandle() == handle)
        {
            gAttachedActionSetHandles.erase(
                std::remove(gAttachedActionSetHandles.begin(), gAttachedActionSetHandles.end(), handle),
                gAttachedActionSetHandles.end());
            gActionSets.erase(it);
            return XR_SUCCESS;
        }
    }
    return XR_ERROR_HANDLE_INVALID;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrCreateAction(
    XrActionSet actionSet, const XrActionCreateInfo* createInfo, XrAction* action)
{
    auto* as = Runtime::Get().FromHandle<ActionSetState>(reinterpret_cast<uint64_t>(actionSet));
    if (as == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || action == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->type != XR_TYPE_ACTION_CREATE_INFO)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (IsAttachedActionSetHandle(as->GetHandle()))
    {
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }
    if (createInfo->actionName[0] == '\0')
    {
        return XR_ERROR_NAME_INVALID;
    }
    if (createInfo->localizedActionName[0] == '\0')
    {
        return XR_ERROR_LOCALIZED_NAME_INVALID;
    }
    if (!IsValidSingleLevelPathName(createInfo->actionName))
    {
        return XR_ERROR_PATH_FORMAT_INVALID;
    }
    if (createInfo->countSubactionPaths > 0 && createInfo->subactionPaths == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::unordered_set<uint64_t> seenSubactionPaths;
    for (uint32_t i = 0; i < createInfo->countSubactionPaths; ++i)
    {
        const XrPath subactionPath = createInfo->subactionPaths[i];
        const std::string pathString = Runtime::Get().GetPathString(subactionPath);
        if (pathString.empty())
        {
            return XR_ERROR_PATH_INVALID;
        }
        if (!IsSupportedActionSubactionPath(pathString) ||
            !seenSubactionPaths.insert(static_cast<uint64_t>(subactionPath)).second)
        {
            return XR_ERROR_PATH_UNSUPPORTED;
        }
    }

    for (const auto& existingAction : gActions)
    {
        if (existingAction->GetActionSet() != as)
        {
            continue;
        }
        if (existingAction->GetName() == createInfo->actionName)
        {
            return XR_ERROR_NAME_DUPLICATED;
        }
        if (existingAction->GetLocalizedName() == createInfo->localizedActionName)
        {
            return XR_ERROR_LOCALIZED_NAME_DUPLICATED;
        }
    }

    auto act = std::make_unique<ActionState>(as, createInfo);
    *action = reinterpret_cast<XrAction>(act->GetHandle());
    gActions.push_back(std::move(act));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings)
{
    auto* inst = GetInstance(instance);
    if (!inst || suggestedBindings == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (suggestedBindings->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (suggestedBindings->countSuggestedBindings == 0 || suggestedBindings->suggestedBindings == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (gActionSetsAttached)
    {
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }

    XrPath profilePath = suggestedBindings->interactionProfile;
    const std::string profilePathString = Runtime::Get().GetPathString(profilePath);
    if (profilePathString.empty())
    {
        return XR_ERROR_PATH_INVALID;
    }
    if (!IsKnownInteractionProfilePath(inst, profilePathString))
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }

    std::vector<SuggestedBinding> bindings;
    bindings.reserve(suggestedBindings->countSuggestedBindings);

    for (uint32_t i = 0; i < suggestedBindings->countSuggestedBindings; i++)
    {
        const auto& b = suggestedBindings->suggestedBindings[i];
        auto* action = Runtime::Get().FromHandle<ActionState>(reinterpret_cast<uint64_t>(b.action));
        if (action == nullptr)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        uint64_t actionHandle = reinterpret_cast<uint64_t>(b.action);
        std::string pathStr = Runtime::Get().GetPathString(b.binding);
        if (pathStr.empty())
        {
            return XR_ERROR_PATH_INVALID;
        }
        if (!IsValidBindingPathForProfile(inst, profilePathString, pathStr))
        {
            return XR_ERROR_PATH_UNSUPPORTED;
        }

        size_t handEnd = pathStr.find("/input/");
        std::string topLevelPathString = (handEnd == std::string::npos) ? "" : pathStr.substr(0, handEnd);
        if (topLevelPathString.empty())
        {
            size_t outputEnd = pathStr.find("/output/");
            topLevelPathString = (outputEnd == std::string::npos) ? "" : pathStr.substr(0, outputEnd);
        }
        XrPath topLevelPath = XR_NULL_PATH;
        if (!topLevelPathString.empty())
        {
            Runtime::Get().StringToPath(topLevelPathString.c_str(), &topLevelPath);
        }

        SuggestedBinding binding = {};
        binding.actionHandle = actionHandle;
        binding.bindingPath = b.binding;
        binding.topLevelPath = topLevelPath;
        binding.profilePathString = profilePathString;
        binding.bindingPathString = pathStr;
        binding.componentPath = ComponentFromBindingPath(pathStr);
        bindings.push_back(std::move(binding));
        spdlog::debug("OXRSys: Binding {} -> {}", actionHandle, pathStr);
    }

    gSuggestedBindings[static_cast<uint64_t>(profilePath)] = std::move(bindings);
    spdlog::info("OXRSys: Stored {} bindings for profile {}",
                  suggestedBindings->countSuggestedBindings,
                  profilePathString);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrAttachSessionActionSets(
    XrSession session, const XrSessionActionSetsAttachInfo* attachInfo)
{
    auto* sess = GetSession(session);
    if (sess == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (attachInfo == nullptr || attachInfo->countActionSets == 0 || attachInfo->actionSets == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (gActionSetsAttached)
    {
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }

    gAttachedActionSetHandles.clear();
    gAttachedActionSetHandles.reserve(attachInfo->countActionSets);
    for (uint32_t i = 0; i < attachInfo->countActionSets; i++)
    {
        auto* actionSet = Runtime::Get().FromHandle<ActionSetState>(
            reinterpret_cast<uint64_t>(attachInfo->actionSets[i]));
        if (actionSet == nullptr)
        {
            gAttachedActionSetHandles.clear();
            return XR_ERROR_HANDLE_INVALID;
        }
        gAttachedActionSetHandles.push_back(reinterpret_cast<uint64_t>(attachInfo->actionSets[i]));
    }
    gActionSetsAttached = true;

    spdlog::info("OXRSys: Attached {} action sets", attachInfo->countActionSets);
    return XR_SUCCESS;
}

// Helper: determine hand from a binding path string
static InputManager::Hand HandFromBindingPath(const std::string& path)
{
    if (path.find("/user/hand/right") != std::string::npos)
    {
        return InputManager::Hand::Right;
    }
    return InputManager::Hand::Left;
}

// Helper: extract the component from a binding path (e.g. "select/click", "grip/pose")
static std::string ComponentFromBindingPath(const std::string& path)
{
    // Path format: /user/hand/<side>/input/<component>
    auto inputPos = path.find("/input/");
    if (inputPos != std::string::npos)
    {
        return path.substr(inputPos + 7); // skip "/input/"
    }
    return "";
}

static bool HandFromTopLevelPath(XrPath topLevelPath, InputManager::Hand* hand)
{
    std::string pathStr = Runtime::Get().GetPathString(topLevelPath);
    if (pathStr == "/user/hand/left")
    {
        *hand = InputManager::Hand::Left;
        return true;
    }
    if (pathStr == "/user/hand/right")
    {
        *hand = InputManager::Hand::Right;
        return true;
    }
    return false;
}

static std::string NormalizeInputSourceComponentPath(XrPath inputSourcePath)
{
    std::string path = Runtime::Get().GetPathString(inputSourcePath);
    if (path.rfind("/input/", 0) == 0)
    {
        return path.substr(7);
    }
    return ComponentFromBindingPath(path);
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

static XrPosef ComposePoses(const XrPosef& basePose, const XrPosef& offsetPose)
{
    glm::quat baseRot = ToGlm(basePose.orientation);
    glm::vec3 basePos = ToGlm(basePose.position);
    glm::quat offsetRot = ToGlm(offsetPose.orientation);
    glm::vec3 offsetPos = ToGlm(offsetPose.position);

    XrPosef pose = {};
    pose.orientation = ToXr(baseRot * offsetRot);
    pose.position = ToXr(basePos + baseRot * offsetPos);
    return pose;
}

static XrPosef ResolveSpaceWorldPose(Space* space)
{
    XrPosef worldPose = {};
    worldPose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    worldPose.position = {0.0f, 0.0f, 0.0f};

    const InputManager& inputManager = space->GetSession()->GetInputManager();

    if (space->GetType() == Space::Type::Reference)
    {
        switch (space->GetReferenceSpaceType())
        {
            case XR_REFERENCE_SPACE_TYPE_VIEW:
                worldPose = inputManager.GetHeadPose();
                break;

            case XR_REFERENCE_SPACE_TYPE_LOCAL:
            case XR_REFERENCE_SPACE_TYPE_STAGE:
            default:
                break;
        }
    }
    else
    {
        auto* action = Runtime::Get().FromHandle<ActionState>(
            reinterpret_cast<uint64_t>(space->GetAction()));
        std::string poseBindingPath;
        std::string poseProfilePath;
        if (action != nullptr)
        {
            const auto& data = action->GetSubactionData(space->GetSubactionPath());
            poseBindingPath = Runtime::Get().GetPathString(data.poseSourcePath);
            poseProfilePath = data.poseSourceProfile;
        }

        if (!poseBindingPath.empty())
        {
            InputManager::Hand hand = HandFromBindingPath(poseBindingPath);
            worldPose = inputManager.GetPoseComponentForProfile(
                hand, ComponentFromBindingPath(poseBindingPath), poseProfilePath);
        }
        else
        {
            worldPose = inputManager.GetControllerPose(
                (space->GetSubactionPath() == XR_NULL_PATH) ? InputManager::Hand::Left
                                                            : HandFromBindingPath(
                                                                  Runtime::Get().GetPathString(space->GetSubactionPath())));
        }
    }

    return ComposePoses(worldPose, space->GetPoseInSpace());
}

static XrPath TopLevelPathFromHand(InputManager::Hand hand)
{
    XrPath path = XR_NULL_PATH;
    Runtime::Get().StringToPath(
        (hand == InputManager::Hand::Left) ? "/user/hand/left" : "/user/hand/right",
        &path);
    return path;
}

static bool IsAttachedActionSetHandle(uint64_t actionSetHandle)
{
    return gActionSetsAttached &&
           std::find(gAttachedActionSetHandles.begin(), gAttachedActionSetHandles.end(), actionSetHandle) !=
               gAttachedActionSetHandles.end();
}

static bool IsActionAttached(const ActionState* action)
{
    return action != nullptr && IsAttachedActionSetHandle(action->GetActionSet()->GetHandle());
}

static XrResult ValidateActionSubactionPath(const ActionState* action, XrPath subactionPath)
{
    if (action == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::vector<XrPath> resolvedSubactionPaths = action->GetResolvedSubactionPaths();
    if (subactionPath == XR_NULL_PATH)
    {
        return XR_SUCCESS;
    }

    if (Runtime::Get().GetPathString(subactionPath).empty())
    {
        return XR_ERROR_PATH_INVALID;
    }

    return std::find(resolvedSubactionPaths.begin(), resolvedSubactionPaths.end(), subactionPath) !=
                   resolvedSubactionPaths.end()
               ? XR_SUCCESS
               : XR_ERROR_PATH_UNSUPPORTED;
}

static ActionState::SubactionData GetQueriedActionState(const ActionState* action, XrPath subactionPath)
{
    if (subactionPath != XR_NULL_PATH)
    {
        return action->GetSubactionData(subactionPath);
    }

    ActionState::SubactionData aggregate = {};
    for (XrPath resolvedSubactionPath : action->GetResolvedSubactionPaths())
    {
        const auto& data = action->GetSubactionData(resolvedSubactionPath);
        aggregate.isActive = aggregate.isActive || data.isActive;
        aggregate.boolValue = aggregate.boolValue || data.boolValue;
        aggregate.boolChanged = aggregate.boolChanged || data.boolChanged;
        aggregate.floatValue = std::max(aggregate.floatValue, data.floatValue);
        aggregate.floatChanged = aggregate.floatChanged || data.floatChanged;
        if (std::fabs(data.vector2fValue.x) > std::fabs(aggregate.vector2fValue.x) ||
            std::fabs(data.vector2fValue.y) > std::fabs(aggregate.vector2fValue.y))
        {
            aggregate.vector2fValue = data.vector2fValue;
        }
        aggregate.vector2fChanged = aggregate.vector2fChanged || data.vector2fChanged;
        aggregate.poseActive = aggregate.poseActive || data.poseActive;
        if (aggregate.poseSourcePath == XR_NULL_PATH && data.poseSourcePath != XR_NULL_PATH)
        {
            aggregate.poseSourcePath = data.poseSourcePath;
            aggregate.poseSourceProfile = data.poseSourceProfile;
        }
        aggregate.lastChangeTime = std::max(aggregate.lastChangeTime, data.lastChangeTime);
        aggregate.boundSources.insert(aggregate.boundSources.end(),
                                      data.boundSources.begin(),
                                      data.boundSources.end());
    }

    return aggregate;
}

static XrResult ValidateActionStateQuery(
    XrSession session,
    const XrActionStateGetInfo* getInfo,
    XrActionType expectedActionType,
    ActionState** actionOut)
{
    auto* sess = GetSession(session);
    if (sess == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (getInfo == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    auto* action = Runtime::Get().FromHandle<ActionState>(reinterpret_cast<uint64_t>(getInfo->action));
    if (action == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!IsActionAttached(action))
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }

    XrResult subactionValidation = ValidateActionSubactionPath(action, getInfo->subactionPath);
    if (subactionValidation != XR_SUCCESS)
    {
        return subactionValidation;
    }
    if (action->GetType() != expectedActionType)
    {
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    }

    *actionOut = action;
    return XR_SUCCESS;
}

static bool IsActionSetActive(const XrActionsSyncInfo* syncInfo, ActionState* action)
{
    if (syncInfo->countActiveActionSets == 0 || syncInfo->activeActionSets == nullptr)
    {
        return std::find(gAttachedActionSetHandles.begin(), gAttachedActionSetHandles.end(),
                          action->GetActionSet()->GetHandle()) != gAttachedActionSetHandles.end();
    }

    uint64_t actionSetHandle = action->GetActionSet()->GetHandle();
    for (uint32_t i = 0; i < syncInfo->countActiveActionSets; i++)
    {
        if (reinterpret_cast<uint64_t>(syncInfo->activeActionSets[i].actionSet) == actionSetHandle)
        {
            return true;
        }
    }
    return false;
}

static int BindingSourcePriority(const std::string& profilePath)
{
    if (profilePath == "/interaction_profiles/ext/hand_interaction_ext")
    {
        return 1;
    }
    return 2;
}

static void AccumulateBindingState(const InputManager& inputManager, const SuggestedBinding& binding,
                                    ActionState* action, XrPath subactionPath,
                                    AggregatedActionState& aggregate)
{
    if (binding.topLevelPath == XR_NULL_PATH)
    {
        return;
    }

    InputManager::Hand hand = HandFromBindingPath(binding.bindingPathString);
    bool deviceActive = inputManager.IsInputDeviceActive(hand);
    if (inputManager.IsStreaming())
    {
        deviceActive = binding.profilePathString == "/interaction_profiles/ext/hand_interaction_ext"
                           ? inputManager.IsHandTrackingActive(hand)
                           : inputManager.IsControllerTrackingActive(hand);
    }
    if (!deviceActive)
    {
        return;
    }

    const int sourcePriority = BindingSourcePriority(binding.profilePathString);
    if (aggregate.sourcePriority > sourcePriority)
    {
        return;
    }
    if (aggregate.sourcePriority < sourcePriority)
    {
        aggregate = {};
        aggregate.sourcePriority = sourcePriority;
    }

    aggregate.isActive = true;
    aggregate.boundSources.push_back(binding.bindingPath);

    switch (action->GetType())
    {
        case XR_ACTION_TYPE_BOOLEAN_INPUT:
            aggregate.boolValue = aggregate.boolValue ||
                                  inputManager.GetBooleanComponentForProfile(
                                      hand, binding.componentPath, binding.profilePathString);
            break;

        case XR_ACTION_TYPE_FLOAT_INPUT:
            aggregate.floatValue = std::max(aggregate.floatValue,
                                            inputManager.GetFloatComponentForProfile(
                                                hand, binding.componentPath, binding.profilePathString));
            break;

        case XR_ACTION_TYPE_VECTOR2F_INPUT:
        {
            XrVector2f vec = inputManager.GetVector2fComponentForProfile(
                hand, binding.componentPath, binding.profilePathString);
            if (std::fabs(vec.x) > std::fabs(aggregate.vector2fValue.x) ||
                std::fabs(vec.y) > std::fabs(aggregate.vector2fValue.y))
            {
                aggregate.vector2fValue = vec;
            }
            break;
        }

        case XR_ACTION_TYPE_POSE_INPUT:
            aggregate.poseActive = true;
            if (aggregate.poseSourcePath == XR_NULL_PATH)
            {
                aggregate.poseSourcePath = binding.bindingPath;
                aggregate.poseSourceProfile = binding.profilePathString;
            }
            break;

        default:
            break;
    }

    (void)subactionPath;
}

static std::string SelectCurrentInteractionProfileForInstance(
    const Instance* instance, const InputManager& inputManager, InputManager::Hand hand)
{
    for (const std::string& profilePath : inputManager.GetCurrentInteractionProfileCandidates(hand))
    {
        if (!profilePath.empty() && IsKnownInteractionProfilePath(instance, profilePath))
        {
            return profilePath;
        }
    }

    if (inputManager.IsControllerTrackingActive(hand) &&
        IsKnownInteractionProfilePath(instance, "/interaction_profiles/oculus/touch_controller"))
    {
        return "/interaction_profiles/oculus/touch_controller";
    }

    return "";
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetCurrentInteractionProfile(
    XrSession session, XrPath topLevelUserPath,
    XrInteractionProfileState* interactionProfile)
{
    auto* sess = GetSession(session);
    if (sess == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (interactionProfile == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!gActionSetsAttached)
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }

    interactionProfile->type = XR_TYPE_INTERACTION_PROFILE_STATE;

    std::string pathStr = Runtime::Get().GetPathString(topLevelUserPath);
    if (pathStr.empty())
    {
        return XR_ERROR_PATH_INVALID;
    }
    if (pathStr == "/user/hand/left" || pathStr == "/user/hand/right")
    {
        InputManager::Hand hand = (pathStr == "/user/hand/right")
                                      ? InputManager::Hand::Right
                                      : InputManager::Hand::Left;
        std::string profilePath = SelectCurrentInteractionProfileForInstance(
            sess->GetInstance(), sess->GetInputManager(), hand);
        if (profilePath.empty())
        {
            interactionProfile->interactionProfile = XR_NULL_PATH;
        }
        else
        {
            Runtime::Get().StringToPath(profilePath.c_str(), &interactionProfile->interactionProfile);
        }
    }
    else
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }

    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrSyncActions(
    XrSession session, const XrActionsSyncInfo* syncInfo)
{
    auto* sess = GetSession(session);
    if (!sess || syncInfo == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!gActionSetsAttached)
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }

    const InputManager& inputManager = sess->GetInputManager();
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, AggregatedActionState>> aggregatedStates;

    for (InputManager::Hand hand : {InputManager::Hand::Left, InputManager::Hand::Right})
    {
        XrPath expectedTopLevelPath = TopLevelPathFromHand(hand);
        for (const std::string& profilePathString : inputManager.GetActiveInteractionProfiles(hand))
        {
            if (profilePathString.empty())
            {
                continue;
            }
            XrPath profilePath = XR_NULL_PATH;
            Runtime::Get().StringToPath(profilePathString.c_str(), &profilePath);

            auto profileIt = gSuggestedBindings.find(static_cast<uint64_t>(profilePath));
            if (profileIt == gSuggestedBindings.end())
            {
                continue;
            }

            for (const auto& binding : profileIt->second)
            {
                if (binding.topLevelPath != expectedTopLevelPath)
                {
                    continue;
                }

                auto* action = Runtime::Get().FromHandle<ActionState>(binding.actionHandle);
                if (!action || !IsActionSetActive(syncInfo, action))
                {
                    continue;
                }

                XrPath subactionPath = action->GetSubactionPaths().empty() ? XR_NULL_PATH : binding.topLevelPath;
                AccumulateBindingState(inputManager, binding, action, subactionPath,
                                        aggregatedStates[binding.actionHandle][static_cast<uint64_t>(subactionPath)]);
            }
        }
    }

    XrTime syncTime = sess->GetCurrentTime();
    for (const auto& actionHolder : gActions)
    {
        ActionState* action = actionHolder.get();
        if (!IsActionSetActive(syncInfo, action))
        {
            continue;
        }

        auto actionIt = aggregatedStates.find(action->GetHandle());
        for (XrPath subactionPath : action->GetResolvedSubactionPaths())
        {
            const AggregatedActionState* aggregate = nullptr;
            if (actionIt != aggregatedStates.end())
            {
                auto subactionIt = actionIt->second.find(static_cast<uint64_t>(subactionPath));
                if (subactionIt != actionIt->second.end())
                {
                    aggregate = &subactionIt->second;
                }
            }
            action->ApplySyncState(subactionPath, aggregate, syncTime);
        }
    }

    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetActionStateBoolean(
    XrSession session, const XrActionStateGetInfo* getInfo,
    XrActionStateBoolean* state)
{
    if (state == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    ActionState* action = nullptr;
    XrResult validation = ValidateActionStateQuery(session, getInfo, XR_ACTION_TYPE_BOOLEAN_INPUT, &action);
    if (validation != XR_SUCCESS)
    {
        return validation;
    }

    ActionState::SubactionData data = GetQueriedActionState(action, getInfo->subactionPath);

    state->type = XR_TYPE_ACTION_STATE_BOOLEAN;
    state->currentState = data.boolValue ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = data.boolChanged ? XR_TRUE : XR_FALSE;
    state->lastChangeTime = data.lastChangeTime;
    state->isActive = data.isActive ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetActionStateFloat(
    XrSession session, const XrActionStateGetInfo* getInfo,
    XrActionStateFloat* state)
{
    if (state == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    ActionState* action = nullptr;
    XrResult validation = ValidateActionStateQuery(session, getInfo, XR_ACTION_TYPE_FLOAT_INPUT, &action);
    if (validation != XR_SUCCESS)
    {
        return validation;
    }

    ActionState::SubactionData data = GetQueriedActionState(action, getInfo->subactionPath);

    state->type = XR_TYPE_ACTION_STATE_FLOAT;
    state->currentState = data.floatValue;
    state->changedSinceLastSync = data.floatChanged ? XR_TRUE : XR_FALSE;
    state->lastChangeTime = data.lastChangeTime;
    state->isActive = data.isActive ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetActionStatePose(
    XrSession session, const XrActionStateGetInfo* getInfo,
    XrActionStatePose* state)
{
    if (state == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    ActionState* action = nullptr;
    XrResult validation = ValidateActionStateQuery(session, getInfo, XR_ACTION_TYPE_POSE_INPUT, &action);
    if (validation != XR_SUCCESS)
    {
        return validation;
    }

    ActionState::SubactionData data = GetQueriedActionState(action, getInfo->subactionPath);

    state->type = XR_TYPE_ACTION_STATE_POSE;
    state->isActive = data.poseActive ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrCreateActionSpace(
    XrSession session, const XrActionSpaceCreateInfo* createInfo, XrSpace* space)
{
    auto* sess = GetSession(session);
    if (!sess || createInfo == nullptr || space == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Determine the subaction path — either explicitly provided or infer from bindings
    XrPath subactionPath = createInfo->subactionPath;

    // If no subaction path, try to infer from the action's bindings
    if (subactionPath == XR_NULL_PATH)
    {
        auto* action = Runtime::Get().FromHandle<ActionState>(
            reinterpret_cast<uint64_t>(createInfo->action));
        if (action && !action->GetSubactionPaths().empty())
        {
            subactionPath = action->GetSubactionPaths()[0];
        }
    }

    auto* action = Runtime::Get().FromHandle<ActionState>(reinterpret_cast<uint64_t>(createInfo->action));
    if (!action)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (action->GetType() != XR_ACTION_TYPE_POSE_INPUT)
    {
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    }

    return sess->CreateActionSpace(createInfo->action, subactionPath, createInfo->poseInActionSpace, space);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrApplyHapticFeedback(
    XrSession session, const XrHapticActionInfo* hapticActionInfo,
    const XrHapticBaseHeader* hapticFeedback)
{
    auto* sess = GetSession(session);
    if (sess == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (hapticActionInfo == nullptr || hapticFeedback == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    auto* action = Runtime::Get().FromHandle<ActionState>(reinterpret_cast<uint64_t>(hapticActionInfo->action));
    if (action == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!IsActionAttached(action))
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    XrResult subactionValidation = ValidateActionSubactionPath(action, hapticActionInfo->subactionPath);
    if (subactionValidation != XR_SUCCESS)
    {
        return subactionValidation;
    }
    if (action->GetType() != XR_ACTION_TYPE_VIBRATION_OUTPUT)
    {
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    }
    if (sess->GetState() != XR_SESSION_STATE_FOCUSED)
    {
        return XR_SESSION_NOT_FOCUSED;
    }

    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrStopHapticFeedback(
    XrSession session, const XrHapticActionInfo* hapticActionInfo)
{
    auto* sess = GetSession(session);
    if (sess == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (hapticActionInfo == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    auto* action = Runtime::Get().FromHandle<ActionState>(reinterpret_cast<uint64_t>(hapticActionInfo->action));
    if (action == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!IsActionAttached(action))
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    XrResult subactionValidation = ValidateActionSubactionPath(action, hapticActionInfo->subactionPath);
    if (subactionValidation != XR_SUCCESS)
    {
        return subactionValidation;
    }
    if (action->GetType() != XR_ACTION_TYPE_VIBRATION_OUTPUT)
    {
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    }
    if (sess->GetState() != XR_SESSION_STATE_FOCUSED)
    {
        return XR_SESSION_NOT_FOCUSED;
    }

    return XR_SUCCESS;
}

// ============================================================================
// Additional core stubs required by Godot
// ============================================================================

static XRAPI_ATTR XrResult XRAPI_CALL OxrDestroyAction(XrAction action)
{
    uint64_t handle = reinterpret_cast<uint64_t>(action);
    for (auto it = gActions.begin(); it != gActions.end(); ++it)
    {
        if ((*it)->GetHandle() == handle)
        {
            gActions.erase(it);
            return XR_SUCCESS;
        }
    }
    return XR_ERROR_HANDLE_INVALID;
}

static std::string LocalizedUserPath(XrPath sourcePath)
{
    std::string path = Runtime::Get().GetPathString(sourcePath);
    if (path.find("/user/hand/left") != std::string::npos)
    {
        return "Left Hand";
    }
    if (path.find("/user/hand/right") != std::string::npos)
    {
        return "Right Hand";
    }
    return "Unknown User";
}

static std::string LocalizedInteractionProfileName(Session* session, XrPath sourcePath)
{
    std::string path = Runtime::Get().GetPathString(sourcePath);
    InputManager::Hand hand = HandFromBindingPath(path);
    std::string profilePath = session->GetInputManager().GetCurrentInteractionProfile(hand);

    if (profilePath == "/interaction_profiles/khr/simple_controller")
    {
        return "Khronos Simple Controller";
    }
    if (profilePath == "/interaction_profiles/oculus/touch_controller")
    {
        return "Oculus Touch Controller";
    }
    if (profilePath == "/interaction_profiles/meta/touch_controller_quest_1_rift_s")
    {
        return "Meta Quest 1/Rift S Touch Controller";
    }
    if (profilePath == "/interaction_profiles/meta/touch_controller_quest_2")
    {
        return "Meta Quest 2 Touch Controller";
    }
    if (profilePath == "/interaction_profiles/meta/touch_plus_controller")
    {
        return "Meta Touch Plus Controller";
    }
    if (profilePath == "/interaction_profiles/meta/touch_controller_plus")
    {
        return "Meta Touch Plus Controller";
    }
    if (profilePath == "/interaction_profiles/bytedance/pico_neo3_controller")
    {
        return "PICO Neo3 Controller";
    }
    if (profilePath == "/interaction_profiles/bytedance/pico4_controller")
    {
        return "PICO 4 Controller";
    }
    if (profilePath == "/interaction_profiles/ext/hand_interaction_ext")
    {
        return "EXT Hand Interaction";
    }
    return "Unknown Profile";
}

static std::string LocalizedComponentName(XrPath sourcePath)
{
    std::string path = Runtime::Get().GetPathString(sourcePath);
    std::string component = ComponentFromBindingPath(path);

    if (component == "select/click" || component == "select/value")
    {
        return "Select";
    }
    if (component == "menu/click")
    {
        return "Menu";
    }
    if (component == "grip/pose")
    {
        return "Grip Pose";
    }
    if (component == "aim/pose")
    {
        return "Aim Pose";
    }
    if (component == "trigger/value" || component == "trigger/click")
    {
        return "Trigger";
    }
    if (component == "squeeze/value")
    {
        return "Squeeze";
    }
    if (component == "thumbstick" || component == "thumbstick/x" || component == "thumbstick/y")
    {
        return "Thumbstick";
    }
    if (component == "a/click" || component == "x/click")
    {
        return "Primary Button";
    }
    if (component == "b/click" || component == "y/click")
    {
        return "Secondary Button";
    }
    if (component == "pinch_ext/pose" || component == "pinch_ext/value")
    {
        return "Pinch";
    }
    if (component == "poke_ext/pose")
    {
        return "Poke";
    }
    if (component == "grasp_ext/value")
    {
        return "Grasp";
    }
    if (component == "aim_activate_ext/value")
    {
        return "Aim Activate";
    }
    return "Unknown Component";
}

static std::string BuildLocalizedInputSourceName(Session* session,
                                                  const XrInputSourceLocalizedNameGetInfo* getInfo)
{
    std::vector<std::string> parts;
    if ((getInfo->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT) != 0)
    {
        parts.push_back(LocalizedUserPath(getInfo->sourcePath));
    }
    if ((getInfo->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT) != 0)
    {
        parts.push_back(LocalizedInteractionProfileName(session, getInfo->sourcePath));
    }
    if ((getInfo->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT) != 0)
    {
        parts.push_back(LocalizedComponentName(getInfo->sourcePath));
    }

    if (parts.empty())
    {
        return "Unknown Source";
    }

    std::string name = parts[0];
    for (size_t i = 1; i < parts.size(); i++)
    {
        name += " ";
        name += parts[i];
    }
    return name;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetActionStateVector2f(
    XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateVector2f* state)
{
    if (getInfo == nullptr || state == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    ActionState* action = nullptr;
    XrResult validation = ValidateActionStateQuery(session, getInfo, XR_ACTION_TYPE_VECTOR2F_INPUT, &action);
    if (validation != XR_SUCCESS)
    {
        return validation;
    }

    ActionState::SubactionData data = GetQueriedActionState(action, getInfo->subactionPath);

    state->type = XR_TYPE_ACTION_STATE_VECTOR2F;
    state->currentState = data.vector2fValue;
    state->changedSinceLastSync = data.vector2fChanged ? XR_TRUE : XR_FALSE;
    state->lastChangeTime = data.lastChangeTime;
    state->isActive = data.isActive ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrEnumerateBoundSourcesForAction(
    XrSession session, const XrBoundSourcesForActionEnumerateInfo* enumerateInfo,
    uint32_t sourceCapacityInput, uint32_t* sourceCountOutput, XrPath* sources)
{
    auto* sess = GetSession(session);
    if (sess == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (enumerateInfo == nullptr || sourceCountOutput == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    auto* action = Runtime::Get().FromHandle<ActionState>(reinterpret_cast<uint64_t>(enumerateInfo->action));
    if (action == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!IsActionAttached(action))
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }

    std::vector<XrPath> boundSources = action->GetBoundSources();
    *sourceCountOutput = static_cast<uint32_t>(boundSources.size());
    if (sourceCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (sourceCapacityInput < boundSources.size())
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    std::copy(boundSources.begin(), boundSources.end(), sources);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetInputSourceLocalizedName(
    XrSession session, const XrInputSourceLocalizedNameGetInfo* getInfo,
    uint32_t bufferCapacityInput, uint32_t* bufferCountOutput, char* buffer)
{
    if (getInfo == nullptr || bufferCountOutput == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::string name = BuildLocalizedInputSourceName(sess, getInfo);
    uint32_t len = static_cast<uint32_t>(name.size() + 1);
    *bufferCountOutput = len;
    if (bufferCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (bufferCapacityInput < len)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    if (buffer)
    {
        memcpy(buffer, name.c_str(), len);
    }
    return XR_SUCCESS;
}

// ============================================================================
// Conformance automation extension (XR_EXT_conformance_automation)
// ============================================================================

static XRAPI_ATTR XrResult XRAPI_CALL OxrSetInputDeviceActiveEXT(
    XrSession session, XrPath interactionProfile, XrPath topLevelPath, XrBool32 isActive)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    InputManager::Hand hand = InputManager::Hand::Left;
    if (!HandFromTopLevelPath(topLevelPath, &hand))
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }

    std::string profilePath = Runtime::Get().GetPathString(interactionProfile);
    if (profilePath.empty())
    {
        return XR_ERROR_PATH_INVALID;
    }
    sess->GetInputManager().SetAutomationInteractionProfile(hand, profilePath, isActive == XR_TRUE);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrSetInputDeviceStateBoolEXT(
    XrSession session, XrPath topLevelPath, XrPath inputSourcePath, XrBool32 state)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    InputManager::Hand hand = InputManager::Hand::Left;
    if (!HandFromTopLevelPath(topLevelPath, &hand))
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }

    std::string componentPath = NormalizeInputSourceComponentPath(inputSourcePath);
    if (componentPath.empty())
    {
        return XR_ERROR_PATH_INVALID;
    }
    sess->GetInputManager().SetAutomationBoolean(hand, componentPath, state == XR_TRUE);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrSetInputDeviceStateFloatEXT(
    XrSession session, XrPath topLevelPath, XrPath inputSourcePath, float state)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    InputManager::Hand hand = InputManager::Hand::Left;
    if (!HandFromTopLevelPath(topLevelPath, &hand))
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }

    std::string componentPath = NormalizeInputSourceComponentPath(inputSourcePath);
    if (componentPath.empty())
    {
        return XR_ERROR_PATH_INVALID;
    }
    sess->GetInputManager().SetAutomationFloat(hand, componentPath, state);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrSetInputDeviceStateVector2fEXT(
    XrSession session, XrPath topLevelPath, XrPath inputSourcePath, XrVector2f state)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    InputManager::Hand hand = InputManager::Hand::Left;
    if (!HandFromTopLevelPath(topLevelPath, &hand))
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }

    std::string componentPath = NormalizeInputSourceComponentPath(inputSourcePath);
    if (componentPath.empty())
    {
        return XR_ERROR_PATH_INVALID;
    }
    sess->GetInputManager().SetAutomationVector2f(hand, componentPath, state);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrSetInputDeviceLocationEXT(
    XrSession session, XrPath topLevelPath, XrPath inputSourcePath, XrSpace space, XrPosef pose)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    InputManager::Hand hand = InputManager::Hand::Left;
    if (!HandFromTopLevelPath(topLevelPath, &hand))
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }

    auto* baseSpace = Runtime::Get().FromHandle<Space>(reinterpret_cast<uint64_t>(space));
    if (baseSpace == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::string componentPath = NormalizeInputSourceComponentPath(inputSourcePath);
    if (componentPath.empty())
    {
        return XR_ERROR_PATH_INVALID;
    }
    XrPosef worldPose = ComposePoses(ResolveSpaceWorldPose(baseSpace), pose);
    sess->GetInputManager().SetAutomationPose(hand, componentPath, worldPose);
    return XR_SUCCESS;
}

// ============================================================================
// Hand tracking extension (XR_EXT_hand_tracking)
// ============================================================================

static XRAPI_ATTR XrResult XRAPI_CALL OxrCreateHandTrackerEXT(
    XrSession session, const XrHandTrackerCreateInfoEXT* createInfo, XrHandTrackerEXT* handTracker)
{
    auto* sess = GetSession(session);
    if (!sess || createInfo == nullptr || handTracker == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!sess->GetInstance()->IsExtensionEnabled(XR_EXT_HAND_TRACKING_EXTENSION_NAME))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }

    auto ht = std::make_unique<HandTracker>(sess, createInfo->hand);
    *handTracker = reinterpret_cast<XrHandTrackerEXT>(ht->GetHandle());
    gHandTrackers.push_back(std::move(ht));

    spdlog::info("OXRSys: Created hand tracker for {}",
                  createInfo->hand == XR_HAND_LEFT_EXT ? "left" : "right");
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrDestroyHandTrackerEXT(XrHandTrackerEXT handTracker)
{
    uint64_t handle = reinterpret_cast<uint64_t>(handTracker);
    for (auto it = gHandTrackers.begin(); it != gHandTrackers.end(); ++it)
    {
        if ((*it)->GetHandle() == handle)
        {
            gHandTrackers.erase(it);
            return XR_SUCCESS;
        }
    }
    return XR_ERROR_HANDLE_INVALID;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrLocateHandJointsEXT(
    XrHandTrackerEXT handTracker, const XrHandJointsLocateInfoEXT* locateInfo,
    XrHandJointLocationsEXT* locations)
{
    auto* ht = Runtime::Get().FromHandle<HandTracker>(reinterpret_cast<uint64_t>(handTracker));
    if (!ht || locateInfo == nullptr || locations == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (!ht->GetSession()->GetInstance()->IsExtensionEnabled(XR_EXT_HAND_TRACKING_EXTENSION_NAME))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (locateInfo->time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }

    return ht->LocateHandJoints(locateInfo->baseSpace, locateInfo->time, locations);
}

// ============================================================================
// Debug utils extension (XR_EXT_debug_utils)
// ============================================================================

static XRAPI_ATTR XrResult XRAPI_CALL OxrSetDebugUtilsObjectNameEXT(
    XrInstance instance, const XrDebugUtilsObjectNameInfoEXT* nameInfo)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!inst->IsExtensionEnabled(XR_EXT_DEBUG_UTILS_EXTENSION_NAME))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (nameInfo == nullptr || nameInfo->objectName == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    inst->SetDebugUtilsObjectName(nameInfo->objectType, nameInfo->objectHandle, nameInfo->objectName);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrCreateDebugUtilsMessengerEXT(
    XrInstance instance,
    const XrDebugUtilsMessengerCreateInfoEXT* createInfo,
    XrDebugUtilsMessengerEXT* messenger)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!inst->IsExtensionEnabled(XR_EXT_DEBUG_UTILS_EXTENSION_NAME))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (createInfo == nullptr || messenger == nullptr || createInfo->userCallback == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    DebugUtilsMessengerState* messengerState = CreateDebugUtilsMessengerState(inst, createInfo);
    if (messengerState == nullptr)
    {
        return XR_ERROR_RUNTIME_FAILURE;
    }

    *messenger = reinterpret_cast<XrDebugUtilsMessengerEXT>(messengerState->handle);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrDestroyDebugUtilsMessengerEXT(XrDebugUtilsMessengerEXT messenger)
{
    uint64_t handle = reinterpret_cast<uint64_t>(messenger);
    auto it = std::find_if(gDebugUtilsMessengers.begin(), gDebugUtilsMessengers.end(),
                           [handle](const auto& candidate)
                           {
                               return candidate->handle == handle;
                           });
    if (it == gDebugUtilsMessengers.end())
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    gDebugUtilsMessengers.erase(it);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrSubmitDebugUtilsMessageEXT(
    XrInstance instance,
    XrDebugUtilsMessageSeverityFlagsEXT messageSeverity,
    XrDebugUtilsMessageTypeFlagsEXT messageTypes,
    const XrDebugUtilsMessengerCallbackDataEXT* callbackData)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!inst->IsExtensionEnabled(XR_EXT_DEBUG_UTILS_EXTENSION_NAME))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (callbackData == nullptr || callbackData->message == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (callbackData->objectCount > 0 && callbackData->objects == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    XrDebugUtilsMessengerCallbackDataEXT resolvedCallbackData = {
        XR_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    std::vector<XrDebugUtilsObjectNameInfoEXT> resolvedObjects;
    std::vector<std::string> resolvedObjectNames;
    std::vector<XrDebugUtilsLabelEXT> resolvedSessionLabels;
    std::vector<std::string> resolvedSessionLabelNames;
    ResolveDebugUtilsCallbackData(inst, callbackData, resolvedCallbackData,
                                  resolvedObjects, resolvedObjectNames,
                                  resolvedSessionLabels, resolvedSessionLabelNames);

    DispatchDebugUtilsMessage(inst, messageSeverity, messageTypes, &resolvedCallbackData);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrSessionBeginDebugUtilsLabelRegionEXT(
    XrSession session, const XrDebugUtilsLabelEXT* labelInfo)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!sess->GetInstance()->IsExtensionEnabled(XR_EXT_DEBUG_UTILS_EXTENSION_NAME))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (labelInfo == nullptr || labelInfo->labelName == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    sess->BeginDebugUtilsLabelRegion(*labelInfo);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrSessionEndDebugUtilsLabelRegionEXT(XrSession session)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!sess->GetInstance()->IsExtensionEnabled(XR_EXT_DEBUG_UTILS_EXTENSION_NAME))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }

    sess->EndDebugUtilsLabelRegion();
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrSessionInsertDebugUtilsLabelEXT(
    XrSession session, const XrDebugUtilsLabelEXT* labelInfo)
{
    auto* sess = GetSession(session);
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!sess->GetInstance()->IsExtensionEnabled(XR_EXT_DEBUG_UTILS_EXTENSION_NAME))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (labelInfo == nullptr || labelInfo->labelName == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    sess->InsertDebugUtilsLabel(*labelInfo);
    return XR_SUCCESS;
}

// ============================================================================
// Vulkan extension (XR_KHR_vulkan_enable + XR_KHR_vulkan_enable2)
// ============================================================================

#ifdef XR_USE_GRAPHICS_API_VULKAN

// Helper: ensure Metal device is created when the Vulkan path needs Metal interop
// on macOS. Linux uses pure Vulkan and does not create a Metal device.
static void EnsureMetalDevice()
{
#ifdef XR_USE_GRAPHICS_API_METAL
    if (!gMetalDevice)
    {
        typedef void* (*MTLCreateSystemDefaultDeviceFn)(void);
        void* metalFramework = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_LAZY);
        if (metalFramework)
        {
            auto createDevice = (MTLCreateSystemDefaultDeviceFn)dlsym(metalFramework,
                                                                        "MTLCreateSystemDefaultDevice");
            if (createDevice)
            {
                gMetalDevice = createDevice();
            }
        }
    }
#endif
}

// --- v1 functions (XR_KHR_vulkan_enable) ---

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetVulkanInstanceExtensionsKHR(
    XrInstance instance, XrSystemId /*systemId*/,
    uint32_t bufferCapacityInput, uint32_t* bufferCountOutput, char* buffer)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (bufferCountOutput == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // No additional instance extensions required from the runtime
    // (Godot/apps handle portability enumeration themselves in the v1 path)
    *bufferCountOutput = 1; // just the null terminator
    if (bufferCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (buffer)
    {
        buffer[0] = '\0';
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetVulkanDeviceExtensionsKHR(
    XrInstance instance, XrSystemId /*systemId*/,
    uint32_t bufferCapacityInput, uint32_t* bufferCountOutput, char* buffer)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (bufferCountOutput == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // No additional device extensions required from the runtime
    // (Godot/apps handle portability subset themselves in the v1 path)
    *bufferCountOutput = 1;
    if (bufferCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (buffer)
    {
        buffer[0] = '\0';
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetVulkanGraphicsDeviceKHR(
    XrInstance instance, XrSystemId /*systemId*/,
    VkInstance vkInstance, VkPhysicalDevice* vkPhysicalDevice)
{
    auto* inst = GetInstance(instance);
    if (!inst || vkInstance == VK_NULL_HANDLE || vkPhysicalDevice == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (!EnsureVulkanInstanceDispatch(vkInstance, "xrGetVulkanGraphicsDeviceKHR"))
    {
        return XR_ERROR_RUNTIME_FAILURE;
    }

    uint32_t deviceCount = 0;
    gVulkanDispatch.enumeratePhysicalDevices(vkInstance, &deviceCount, nullptr);
    if (deviceCount == 0)
    {
        spdlog::error("OXRSys: No Vulkan physical devices found");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    gVulkanDispatch.enumeratePhysicalDevices(vkInstance, &deviceCount, devices.data());
    *vkPhysicalDevice = devices[0];

    if (gVulkanDispatch.getPhysicalDeviceProperties)
    {
        VkPhysicalDeviceProperties props;
        gVulkanDispatch.getPhysicalDeviceProperties(*vkPhysicalDevice, &props);
        spdlog::info("OXRSys: Selected Vulkan device: {}", props.deviceName);
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetVulkanGraphicsRequirementsKHR(
    XrInstance instance, XrSystemId systemId,
    XrGraphicsRequirementsVulkanKHR* graphicsRequirements)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != 1)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (graphicsRequirements == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!inst->IsExtensionEnabled(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) &&
        !inst->IsExtensionEnabled(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }

    EnsureMetalDevice();

    graphicsRequirements->type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
    graphicsRequirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    graphicsRequirements->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
    inst->MarkVulkanGraphicsRequirementsQueried();

    spdlog::info("OXRSys: Vulkan graphics requirements provided");
    return XR_SUCCESS;
}

// --- v2 functions (XR_KHR_vulkan_enable2) ---

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetVulkanGraphicsRequirements2KHR(
    XrInstance instance, XrSystemId systemId,
    XrGraphicsRequirementsVulkanKHR* graphicsRequirements)
{
    // v2 is identical to v1 for requirements
    return OxrGetVulkanGraphicsRequirementsKHR(instance, systemId, graphicsRequirements);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrCreateVulkanInstanceKHR(
    XrInstance instance, const XrVulkanInstanceCreateInfoKHR* createInfo,
    VkInstance* vulkanInstance, VkResult* vulkanResult)
{
    auto* inst = GetInstance(instance);
    if (!inst || createInfo == nullptr || vulkanInstance == nullptr || vulkanResult == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    EnsureMetalDevice();

    // Store the app's pfnGetInstanceProcAddr and resolve all Vulkan functions through it.
    // We do NOT link against the Vulkan loader to avoid loading system MoltenVK which
    // would conflict with app-embedded MoltenVK (e.g. Godot).
    gVulkanDispatch.getInstanceProcAddr = createInfo->pfnGetInstanceProcAddr;
    gVulkanDispatch.LoadPreInstanceFunctions();

    if (!gVulkanDispatch.createInstance)
    {
        spdlog::error("OXRSys: Failed to resolve vkCreateInstance from app");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    const VkInstanceCreateInfo* appCreateInfo = createInfo->vulkanCreateInfo;
    VkInstanceCreateInfo modifiedCreateInfo = *appCreateInfo;

    std::vector<const char*> extensions(appCreateInfo->ppEnabledExtensionNames,
                                         appCreateInfo->ppEnabledExtensionNames + appCreateInfo->enabledExtensionCount);

    // Check if VK_KHR_portability_enumeration is available before injecting
    bool portabilityEnumAvailable = false;
    if (gVulkanDispatch.enumerateInstanceExtensionProperties)
    {
        uint32_t extCount = 0;
        gVulkanDispatch.enumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> availableExts(extCount);
        gVulkanDispatch.enumerateInstanceExtensionProperties(nullptr, &extCount, availableExts.data());
        for (const auto& ext : availableExts)
        {
            if (strcmp(ext.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0)
            {
                portabilityEnumAvailable = true;
                break;
            }
        }
    }

    if (portabilityEnumAvailable)
    {
        modifiedCreateInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        bool alreadyPresent = false;
        for (const auto* ext : extensions)
        {
            if (strcmp(ext, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0)
            {
                alreadyPresent = true;
                break;
            }
        }
        if (!alreadyPresent)
        {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        }
    }
    modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();

    *vulkanResult = gVulkanDispatch.createInstance(&modifiedCreateInfo, createInfo->vulkanAllocator,
                                                    vulkanInstance);
    if (*vulkanResult != VK_SUCCESS)
    {
        spdlog::error("OXRSys: vkCreateInstance failed with {}", static_cast<int>(*vulkanResult));
        return XR_ERROR_RUNTIME_FAILURE;
    }

    // Load instance-level functions through the app's dispatch
    gVulkanDispatch.LoadInstanceFunctions(*vulkanInstance);

    spdlog::info("OXRSys: Created Vulkan instance on behalf of app");
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetVulkanGraphicsDevice2KHR(
    XrInstance instance, const XrVulkanGraphicsDeviceGetInfoKHR* getInfo,
    VkPhysicalDevice* vulkanPhysicalDevice)
{
    auto* inst = GetInstance(instance);
    if (!inst || getInfo == nullptr || vulkanPhysicalDevice == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    return OxrGetVulkanGraphicsDeviceKHR(instance, getInfo->systemId,
                                          getInfo->vulkanInstance, vulkanPhysicalDevice);
}

static XRAPI_ATTR XrResult XRAPI_CALL OxrCreateVulkanDeviceKHR(
    XrInstance instance, const XrVulkanDeviceCreateInfoKHR* createInfo,
    VkDevice* vulkanDevice, VkResult* vulkanResult)
{
    auto* inst = GetInstance(instance);
    if (!inst || createInfo == nullptr || vulkanDevice == nullptr || vulkanResult == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Call vkCreateDevice using the app's provided creation info.
    const VkDeviceCreateInfo* appDeviceInfo = createInfo->vulkanCreateInfo;
    VkDeviceCreateInfo modifiedDeviceInfo = *appDeviceInfo;

    std::vector<const char*> deviceExts(appDeviceInfo->ppEnabledExtensionNames,
                                         appDeviceInfo->ppEnabledExtensionNames + appDeviceInfo->enabledExtensionCount);

#if defined(__APPLE__)
    // Inject VK_KHR_portability_subset for MoltenVK on macOS.
    bool hasPortabilitySubset = false;
    for (const auto* ext : deviceExts)
    {
        if (strcmp(ext, "VK_KHR_portability_subset") == 0)
        {
            hasPortabilitySubset = true;
            break;
        }
    }
    if (!hasPortabilitySubset)
    {
        deviceExts.push_back("VK_KHR_portability_subset");
    }

    // Add VK_EXT_metal_objects for MTLTexture extraction (debug rendering)
    bool hasMetalObjects = false;
    for (const auto* ext : deviceExts)
    {
        if (strcmp(ext, VK_EXT_METAL_OBJECTS_EXTENSION_NAME) == 0)
        {
            hasMetalObjects = true;
            break;
        }
    }
    if (!hasMetalObjects)
    {
        deviceExts.push_back(VK_EXT_METAL_OBJECTS_EXTENSION_NAME);
    }
#endif

    modifiedDeviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
    modifiedDeviceInfo.ppEnabledExtensionNames = deviceExts.data();

    if (!gVulkanDispatch.createDevice)
    {
        spdlog::error("OXRSys: vkCreateDevice not resolved");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *vulkanResult = gVulkanDispatch.createDevice(createInfo->vulkanPhysicalDevice,
                                                   &modifiedDeviceInfo,
                                                   createInfo->vulkanAllocator,
                                                   vulkanDevice);
    if (*vulkanResult != VK_SUCCESS)
    {
        spdlog::error("OXRSys: vkCreateDevice failed with {}", static_cast<int>(*vulkanResult));
        return XR_ERROR_RUNTIME_FAILURE;
    }

    spdlog::info("OXRSys: Created Vulkan device on behalf of app");
    return XR_SUCCESS;
}

#endif // XR_USE_GRAPHICS_API_VULKAN

// ============================================================================
// Direct3D extensions (XR_KHR_D3D11_enable + XR_KHR_D3D12_enable)
// ============================================================================

#ifdef OXRSYS_USE_D3D11
static XRAPI_ATTR XrResult XRAPI_CALL OxrGetD3D11GraphicsRequirementsKHR(
    XrInstance instance, XrSystemId systemId,
    XrGraphicsRequirementsD3D11KHR* graphicsRequirements)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != 1)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (graphicsRequirements == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!inst->IsExtensionEnabled(XR_KHR_D3D11_ENABLE_EXTENSION_NAME))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }

    graphicsRequirements->type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
    graphicsRequirements->adapterLuid = GetDefaultAdapterLuid();
    graphicsRequirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    inst->MarkD3D11GraphicsRequirementsQueried();

    spdlog::info("OXRSys: D3D11 graphics requirements provided");
    return XR_SUCCESS;
}
#endif

#ifdef OXRSYS_USE_D3D12
static XRAPI_ATTR XrResult XRAPI_CALL OxrGetD3D12GraphicsRequirementsKHR(
    XrInstance instance, XrSystemId systemId,
    XrGraphicsRequirementsD3D12KHR* graphicsRequirements)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != 1)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (graphicsRequirements == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!inst->IsExtensionEnabled(XR_KHR_D3D12_ENABLE_EXTENSION_NAME))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }

    graphicsRequirements->type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
    graphicsRequirements->adapterLuid = GetDefaultAdapterLuid();
    graphicsRequirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    inst->MarkD3D12GraphicsRequirementsQueried();

    spdlog::info("OXRSys: D3D12 graphics requirements provided");
    return XR_SUCCESS;
}
#endif

// ============================================================================
// Metal extension
// ============================================================================

#ifdef XR_USE_GRAPHICS_API_METAL
static XRAPI_ATTR XrResult XRAPI_CALL OxrGetMetalGraphicsRequirementsKHR(
    XrInstance instance, XrSystemId systemId,
    XrGraphicsRequirementsMetalKHR* graphicsRequirements)
{
    auto* inst = GetInstance(instance);
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != 1)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (graphicsRequirements == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Create or reuse the Metal device
    if (!gMetalDevice)
    {
        // We'll create the device using Objective-C in a helper
        // For now, use MTLCreateSystemDefaultDevice equivalent
        // This is handled in the .mm file but we need a C++ way
        // We'll store the device pointer and the app will use it
        // Actually, we need to call Metal API — let's use dlsym
        typedef void* (*MTLCreateSystemDefaultDeviceFn)(void);
        void* metalFramework = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_LAZY);
        if (metalFramework)
        {
            auto createDevice = (MTLCreateSystemDefaultDeviceFn)dlsym(metalFramework, "MTLCreateSystemDefaultDevice");
            if (createDevice)
            {
                gMetalDevice = createDevice();
            }
        }
    }

    if (!gMetalDevice)
    {
        spdlog::error("OXRSys: Failed to create Metal device");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    graphicsRequirements->type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR;
    graphicsRequirements->metalDevice = gMetalDevice;
    inst->MarkMetalGraphicsRequirementsQueried();

    spdlog::info("OXRSys: Metal graphics requirements provided");
    return XR_SUCCESS;
}
#endif

// ============================================================================
// xrGetInstanceProcAddr — the main dispatch function
// ============================================================================

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetInstanceProcAddr(
    XrInstance instance, const char* name, PFN_xrVoidFunction* function);

// Macro to simplify dispatch
#define DISPATCH(funcName, funcPtr)          \
    if (std::strcmp(name, #funcName) == 0)   \
    {                                        \
        *function = reinterpret_cast<PFN_xrVoidFunction>(funcPtr); \
        return XR_SUCCESS;                   \
    }

static XRAPI_ATTR XrResult XRAPI_CALL OxrGetInstanceProcAddr(
    XrInstance instance, const char* name, PFN_xrVoidFunction* function)
{
    if (name == nullptr || function == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *function = nullptr;

    Instance* inst = nullptr;
    if (instance != XR_NULL_HANDLE)
    {
        inst = GetInstance(instance);
        if (inst == nullptr)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
    }

    if (!IsFunctionEnabledForInstance(inst, name))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }

    // Global functions
    DISPATCH(xrGetInstanceProcAddr, OxrGetInstanceProcAddr)
    DISPATCH(xrEnumerateInstanceExtensionProperties, OxrEnumerateInstanceExtensionProperties)
    DISPATCH(xrEnumerateApiLayerProperties, OxrEnumerateApiLayerProperties)
    DISPATCH(xrCreateInstance, OxrCreateInstance)

    // Instance
    DISPATCH(xrDestroyInstance, OxrDestroyInstance)
    DISPATCH(xrGetInstanceProperties, OxrGetInstanceProperties)
    DISPATCH(xrGetSystem, OxrGetSystem)
    DISPATCH(xrGetSystemProperties, OxrGetSystemProperties)
    DISPATCH(xrPollEvent, OxrPollEvent)
    DISPATCH(xrResultToString, OxrResultToString)
    DISPATCH(xrStructureTypeToString, OxrStructureTypeToString)
    DISPATCH(xrStringToPath, OxrStringToPath)
    DISPATCH(xrPathToString, OxrPathToString)

    // View configuration
    DISPATCH(xrEnumerateViewConfigurations, OxrEnumerateViewConfigurations)
    DISPATCH(xrGetViewConfigurationProperties, OxrGetViewConfigurationProperties)
    DISPATCH(xrEnumerateViewConfigurationViews, OxrEnumerateViewConfigurationViews)
    DISPATCH(xrEnumerateEnvironmentBlendModes, OxrEnumerateEnvironmentBlendModes)

    // Session
    DISPATCH(xrCreateSession, OxrCreateSession)
    DISPATCH(xrDestroySession, OxrDestroySession)
    DISPATCH(xrBeginSession, OxrBeginSession)
    DISPATCH(xrEndSession, OxrEndSession)
    DISPATCH(xrRequestExitSession, OxrRequestExitSession)
    DISPATCH(xrWaitFrame, OxrWaitFrame)
    DISPATCH(xrBeginFrame, OxrBeginFrame)
    DISPATCH(xrEndFrame, OxrEndFrame)
    DISPATCH(xrLocateViews, OxrLocateViews)

    // Swapchain
    DISPATCH(xrEnumerateSwapchainFormats, OxrEnumerateSwapchainFormats)
    DISPATCH(xrCreateSwapchain, OxrCreateSwapchain)
    DISPATCH(xrDestroySwapchain, OxrDestroySwapchain)
    DISPATCH(xrEnumerateSwapchainImages, OxrEnumerateSwapchainImages)
    DISPATCH(xrAcquireSwapchainImage, OxrAcquireSwapchainImage)
    DISPATCH(xrWaitSwapchainImage, OxrWaitSwapchainImage)
    DISPATCH(xrReleaseSwapchainImage, OxrReleaseSwapchainImage)

    // Spaces
    DISPATCH(xrEnumerateReferenceSpaces, OxrEnumerateReferenceSpaces)
    DISPATCH(xrCreateReferenceSpace, OxrCreateReferenceSpace)
    DISPATCH(xrDestroySpace, OxrDestroySpace)
    DISPATCH(xrLocateSpace, OxrLocateSpace)
    DISPATCH(xrLocateSpaces, OxrLocateSpaces)
    DISPATCH(xrLocateSpacesKHR, OxrLocateSpaces)
    DISPATCH(xrGetReferenceSpaceBoundsRect, OxrGetReferenceSpaceBoundsRect)

    // Actions
    DISPATCH(xrCreateActionSet, OxrCreateActionSet)
    DISPATCH(xrDestroyActionSet, OxrDestroyActionSet)
    DISPATCH(xrCreateAction, OxrCreateAction)
    DISPATCH(xrSuggestInteractionProfileBindings, OxrSuggestInteractionProfileBindings)
    DISPATCH(xrAttachSessionActionSets, OxrAttachSessionActionSets)
    DISPATCH(xrGetCurrentInteractionProfile, OxrGetCurrentInteractionProfile)
    DISPATCH(xrSyncActions, OxrSyncActions)
    DISPATCH(xrGetActionStateBoolean, OxrGetActionStateBoolean)
    DISPATCH(xrGetActionStateFloat, OxrGetActionStateFloat)
    DISPATCH(xrGetActionStatePose, OxrGetActionStatePose)
    DISPATCH(xrCreateActionSpace, OxrCreateActionSpace)
    DISPATCH(xrApplyHapticFeedback, OxrApplyHapticFeedback)
    DISPATCH(xrStopHapticFeedback, OxrStopHapticFeedback)
    DISPATCH(xrDestroyAction, OxrDestroyAction)
    DISPATCH(xrGetActionStateVector2f, OxrGetActionStateVector2f)
    DISPATCH(xrEnumerateBoundSourcesForAction, OxrEnumerateBoundSourcesForAction)
    DISPATCH(xrGetInputSourceLocalizedName, OxrGetInputSourceLocalizedName)

    // Conformance automation extension
    DISPATCH(xrSetInputDeviceActiveEXT, OxrSetInputDeviceActiveEXT)
    DISPATCH(xrSetInputDeviceStateBoolEXT, OxrSetInputDeviceStateBoolEXT)
    DISPATCH(xrSetInputDeviceStateFloatEXT, OxrSetInputDeviceStateFloatEXT)
    DISPATCH(xrSetInputDeviceStateVector2fEXT, OxrSetInputDeviceStateVector2fEXT)
    DISPATCH(xrSetInputDeviceLocationEXT, OxrSetInputDeviceLocationEXT)

    // Hand tracking extension
    DISPATCH(xrCreateHandTrackerEXT, OxrCreateHandTrackerEXT)
    DISPATCH(xrDestroyHandTrackerEXT, OxrDestroyHandTrackerEXT)
    DISPATCH(xrLocateHandJointsEXT, OxrLocateHandJointsEXT)

    // Debug utils extension
    DISPATCH(xrSetDebugUtilsObjectNameEXT, OxrSetDebugUtilsObjectNameEXT)
    DISPATCH(xrCreateDebugUtilsMessengerEXT, OxrCreateDebugUtilsMessengerEXT)
    DISPATCH(xrDestroyDebugUtilsMessengerEXT, OxrDestroyDebugUtilsMessengerEXT)
    DISPATCH(xrSubmitDebugUtilsMessageEXT, OxrSubmitDebugUtilsMessageEXT)
    DISPATCH(xrSessionBeginDebugUtilsLabelRegionEXT, OxrSessionBeginDebugUtilsLabelRegionEXT)
    DISPATCH(xrSessionEndDebugUtilsLabelRegionEXT, OxrSessionEndDebugUtilsLabelRegionEXT)
    DISPATCH(xrSessionInsertDebugUtilsLabelEXT, OxrSessionInsertDebugUtilsLabelEXT)

    // Metal extension
#ifdef XR_USE_GRAPHICS_API_METAL
    DISPATCH(xrGetMetalGraphicsRequirementsKHR, OxrGetMetalGraphicsRequirementsKHR)
    if (std::strcmp(name, UNITY_METAL_GRAPHICS_REQUIREMENTS_FUNCTION_ALIAS) == 0)
    {
        *function = reinterpret_cast<PFN_xrVoidFunction>(OxrGetMetalGraphicsRequirementsKHR);
        return XR_SUCCESS;
    }
#endif

#ifdef XR_USE_GRAPHICS_API_VULKAN
    // Vulkan v1 extension
    DISPATCH(xrGetVulkanInstanceExtensionsKHR, OxrGetVulkanInstanceExtensionsKHR)
    DISPATCH(xrGetVulkanDeviceExtensionsKHR, OxrGetVulkanDeviceExtensionsKHR)
    DISPATCH(xrGetVulkanGraphicsDeviceKHR, OxrGetVulkanGraphicsDeviceKHR)
    DISPATCH(xrGetVulkanGraphicsRequirementsKHR, OxrGetVulkanGraphicsRequirementsKHR)

    // Vulkan v2 extension
    DISPATCH(xrCreateVulkanInstanceKHR, OxrCreateVulkanInstanceKHR)
    DISPATCH(xrCreateVulkanDeviceKHR, OxrCreateVulkanDeviceKHR)
    DISPATCH(xrGetVulkanGraphicsDevice2KHR, OxrGetVulkanGraphicsDevice2KHR)
    DISPATCH(xrGetVulkanGraphicsRequirements2KHR, OxrGetVulkanGraphicsRequirements2KHR)
#endif

#ifdef OXRSYS_USE_D3D11
    DISPATCH(xrGetD3D11GraphicsRequirementsKHR, OxrGetD3D11GraphicsRequirementsKHR)
#endif

#ifdef OXRSYS_USE_D3D12
    DISPATCH(xrGetD3D12GraphicsRequirementsKHR, OxrGetD3D12GraphicsRequirementsKHR)
#endif

    spdlog::warn("OXRSys: Unsupported function requested: {}", name);
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

#undef DISPATCH

// ============================================================================
// Loader negotiation — the only exported symbol
// ============================================================================

extern "C"
{
#if defined(_WIN32)
    BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
    {
        (void)module;
        (void)reserved;
        if (reason == DLL_PROCESS_DETACH)
        {
            CleanupRuntimeState();
        }
        return TRUE;
    }
#else
    __attribute__((destructor))
    static void CleanupRuntimeOnUnload()
    {
        CleanupRuntimeState();
    }
#endif

#if defined(_WIN32)
    __declspec(dllexport)
#else
    __attribute__((visibility("default")))
#endif
    XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(
        const XrNegotiateLoaderInfo* loaderInfo,
        XrNegotiateRuntimeRequest* runtimeRequest)
    {
        if (loaderInfo == nullptr || runtimeRequest == nullptr ||
            loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
            loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
            loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
            runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
            runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION ||
            runtimeRequest->structSize != sizeof(XrNegotiateRuntimeRequest) ||
            loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
            loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION ||
            loaderInfo->minApiVersion < XR_MAKE_VERSION(0, 1, 0) ||
            loaderInfo->minApiVersion >= XR_MAKE_VERSION(1, 1, 0))
        {
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
        runtimeRequest->runtimeApiVersion = XR_CURRENT_API_VERSION;
        runtimeRequest->getInstanceProcAddr = OxrGetInstanceProcAddr;

        spdlog::info("OXRSys: Runtime negotiation successful");
        return XR_SUCCESS;
    }
}
