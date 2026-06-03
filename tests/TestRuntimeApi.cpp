// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{

static constexpr const char* UNITY_METAL_ENABLE_EXTENSION_ALIAS = "XR_KHRX2_metal_enable";
static constexpr const char* UNITY_METAL_GRAPHICS_REQUIREMENTS_FUNCTION_ALIAS =
    "xrGetMetalGraphicsRequirementsKHRX2";
static constexpr const char* XR_LOCATE_SPACES_KHR_FUNCTION_ALIAS = "xrLocateSpacesKHR";

void* DummyMetalCommandQueue()
{
    return reinterpret_cast<void*>(static_cast<uintptr_t>(0x1));
}

void CheckXr(XrResult result, const char* expression)
{
    INFO(expression);
    REQUIRE(result == XR_SUCCESS);
}

#define XR_CHECK(expr) CheckXr((expr), #expr)

void CheckXrBeginFrame(XrResult result, const char* expression)
{
    INFO(expression);
    REQUIRE((result == XR_SUCCESS || result == XR_FRAME_DISCARDED));
}

#define XR_BEGIN_FRAME_CHECK(expr) CheckXrBeginFrame((expr), #expr)

PFN_xrVoidFunction GetProc(XrInstance instance, const char* name)
{
    PFN_xrVoidFunction function = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(instance, name, &function));
    REQUIRE(function != nullptr);
    return function;
}

int64_t SelectColorSwapchainFormat(XrSession session)
{
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr));
    REQUIRE(formatCount > 0);

    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()));
    return formats.front();
}

XrSwapchain CreateColorSwapchain(XrSession session, int64_t format, XrSwapchainCreateFlags createFlags = 0)
{
    XrSwapchainCreateInfo createInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.createFlags = createFlags;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = format;
    createInfo.sampleCount = 1;
    createInfo.width = 16;
    createInfo.height = 16;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;

    XrSwapchain swapchain = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSwapchain(session, &createInfo, &swapchain));
    return swapchain;
}

XrBool32 XRAPI_PTR CountingDebugUtilsCallback(
    XrDebugUtilsMessageSeverityFlagsEXT /*messageSeverity*/,
    XrDebugUtilsMessageTypeFlagsEXT /*messageTypes*/,
    const XrDebugUtilsMessengerCallbackDataEXT* /*callbackData*/,
    void* userData)
{
    auto* callbackCount = static_cast<uint32_t*>(userData);
    if (callbackCount == nullptr)
    {
        return XR_FALSE;
    }
    ++(*callbackCount);
    return XR_FALSE;
}

struct CapturedDebugUtilsMessage
{
    std::string messageId;
    std::vector<std::string> objectNames;
    std::vector<std::string> sessionLabels;
};

XrBool32 XRAPI_PTR CaptureDebugUtilsCallback(
    XrDebugUtilsMessageSeverityFlagsEXT /*messageSeverity*/,
    XrDebugUtilsMessageTypeFlagsEXT /*messageTypes*/,
    const XrDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData)
{
    auto* capturedMessage = static_cast<CapturedDebugUtilsMessage*>(userData);
    if (capturedMessage == nullptr || callbackData == nullptr)
    {
        return XR_FALSE;
    }

    capturedMessage->messageId = callbackData->messageId != nullptr ? callbackData->messageId : "";
    capturedMessage->objectNames.clear();
    capturedMessage->sessionLabels.clear();

    for (uint32_t i = 0; i < callbackData->objectCount; ++i)
    {
        const char* objectName = callbackData->objects[i].objectName;
        capturedMessage->objectNames.emplace_back(objectName != nullptr ? objectName : "");
    }

    for (uint32_t i = 0; i < callbackData->sessionLabelCount; ++i)
    {
        const char* labelName = callbackData->sessionLabels[i].labelName;
        capturedMessage->sessionLabels.emplace_back(labelName != nullptr ? labelName : "");
    }

    return XR_FALSE;
}

struct RuntimeSessionContext
{
    explicit RuntimeSessionContext(std::initializer_list<const char*> extensions, bool beginSession = true)
        : enabledExtensions(extensions)
    {
        XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
        std::strncpy(createInfo.applicationInfo.applicationName, "oxrsys_runtime_api_tests",
                     XR_MAX_APPLICATION_NAME_SIZE);
        createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
        createInfo.enabledExtensionNames = enabledExtensions.data();
        XR_CHECK(xrCreateInstance(&createInfo, &instance));

        getMetalGraphicsRequirementsKHR =
            reinterpret_cast<PFN_xrGetMetalGraphicsRequirementsKHR>(
                GetProc(instance, "xrGetMetalGraphicsRequirementsKHR"));

        if (HasExtension(XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME))
        {
            setInputDeviceActiveEXT =
                reinterpret_cast<PFN_xrSetInputDeviceActiveEXT>(
                    GetProc(instance, "xrSetInputDeviceActiveEXT"));
            setInputDeviceStateBoolEXT =
                reinterpret_cast<PFN_xrSetInputDeviceStateBoolEXT>(
                    GetProc(instance, "xrSetInputDeviceStateBoolEXT"));
            setInputDeviceStateFloatEXT =
                reinterpret_cast<PFN_xrSetInputDeviceStateFloatEXT>(
                    GetProc(instance, "xrSetInputDeviceStateFloatEXT"));
            setInputDeviceStateVector2fEXT =
                reinterpret_cast<PFN_xrSetInputDeviceStateVector2fEXT>(
                    GetProc(instance, "xrSetInputDeviceStateVector2fEXT"));
            setInputDeviceLocationEXT =
                reinterpret_cast<PFN_xrSetInputDeviceLocationEXT>(
                    GetProc(instance, "xrSetInputDeviceLocationEXT"));
        }

        XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
        systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XR_CHECK(xrGetSystem(instance, &systemGetInfo, &systemId));

        XrGraphicsRequirementsMetalKHR graphicsRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
        XR_CHECK(getMetalGraphicsRequirementsKHR(instance, systemId, &graphicsRequirements));

        XrGraphicsBindingMetalKHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
        graphicsBinding.commandQueue = DummyMetalCommandQueue();

        XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
        sessionCreateInfo.next = &graphicsBinding;
        sessionCreateInfo.systemId = systemId;
        XR_CHECK(xrCreateSession(instance, &sessionCreateInfo, &session));

        if (beginSession)
        {
            XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XR_CHECK(xrBeginSession(session, &beginInfo));

            XrReferenceSpaceCreateInfo referenceSpaceCreateInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            referenceSpaceCreateInfo.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            XR_CHECK(xrCreateReferenceSpace(session, &referenceSpaceCreateInfo, &localSpace));
        }
    }

    ~RuntimeSessionContext()
    {
        if (localSpace != XR_NULL_HANDLE)
        {
            xrDestroySpace(localSpace);
        }
        if (session != XR_NULL_HANDLE)
        {
            xrDestroySession(session);
        }
        if (instance != XR_NULL_HANDLE)
        {
            xrDestroyInstance(instance);
        }
    }

    bool HasExtension(const char* extensionName) const
    {
        for (const char* enabledExtension : enabledExtensions)
        {
            if (std::strcmp(enabledExtension, extensionName) == 0)
            {
                return true;
            }
        }
        return false;
    }

    XrPath Path(const char* pathString) const
    {
        XrPath path = XR_NULL_PATH;
        XR_CHECK(xrStringToPath(instance, pathString, &path));
        return path;
    }

    std::vector<const char*> enabledExtensions;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;

    PFN_xrGetMetalGraphicsRequirementsKHR getMetalGraphicsRequirementsKHR = nullptr;
    PFN_xrSetInputDeviceActiveEXT setInputDeviceActiveEXT = nullptr;
    PFN_xrSetInputDeviceStateBoolEXT setInputDeviceStateBoolEXT = nullptr;
    PFN_xrSetInputDeviceStateFloatEXT setInputDeviceStateFloatEXT = nullptr;
    PFN_xrSetInputDeviceStateVector2fEXT setInputDeviceStateVector2fEXT = nullptr;
    PFN_xrSetInputDeviceLocationEXT setInputDeviceLocationEXT = nullptr;
};

} // namespace

TEST_CASE("Loader-backed runtime exposes CTS-focused extensions", "[runtime][loader]")
{
    uint32_t extensionCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
    REQUIRE(extensionCount >= 5);

    std::vector<XrExtensionProperties> properties(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount,
                                                    properties.data()));

    auto hasExtension = [&](const char* extensionName)
    {
        for (const auto& property : properties)
        {
            if (std::strcmp(property.extensionName, extensionName) == 0)
            {
                return true;
            }
        }
        return false;
    };

    CHECK(hasExtension(XR_KHR_METAL_ENABLE_EXTENSION_NAME));
    CHECK(hasExtension(UNITY_METAL_ENABLE_EXTENSION_ALIAS));
    CHECK(hasExtension(XR_EXT_HAND_TRACKING_EXTENSION_NAME));
    CHECK(hasExtension(XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME));
    CHECK(hasExtension(XR_EXT_HAND_INTERACTION_EXTENSION_NAME));
    CHECK(hasExtension(XR_EXT_DEBUG_UTILS_EXTENSION_NAME));
}

TEST_CASE("Loader-backed runtime reports configured product version", "[runtime][loader]")
{
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "oxrsys_runtime_version_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));

    XrInstanceProperties properties = {XR_TYPE_INSTANCE_PROPERTIES};
    XR_CHECK(xrGetInstanceProperties(instance, &properties));
    CHECK(properties.runtimeVersion ==
          XR_MAKE_VERSION(OXRSYS_VERSION_MAJOR, OXRSYS_VERSION_MINOR, OXRSYS_VERSION_PATCH));
    CHECK(std::string(properties.runtimeName) == "OXRSys Runtime");

    xrDestroyInstance(instance);
}

TEST_CASE("Runtime accepts Unity metal extension alias", "[runtime][loader]")
{
    std::array<const char*, 2> enabledExtensions = {
        UNITY_METAL_ENABLE_EXTENSION_ALIAS,
        XR_EXT_HAND_INTERACTION_EXTENSION_NAME,
    };

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "unity_alias_runtime_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));

    PFN_xrGetMetalGraphicsRequirementsKHR getMetalGraphicsRequirementsKHR =
        reinterpret_cast<PFN_xrGetMetalGraphicsRequirementsKHR>(
            GetProc(instance, "xrGetMetalGraphicsRequirementsKHR"));

    XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance, &systemGetInfo, &systemId));

    XrGraphicsRequirementsMetalKHR graphicsRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
    XR_CHECK(getMetalGraphicsRequirementsKHR(instance, systemId, &graphicsRequirements));

    xrDestroyInstance(instance);
}

TEST_CASE("Runtime exposes Unity metal graphics requirements alias", "[runtime][loader]")
{
    std::array<const char*, 1> enabledExtensions = {
        UNITY_METAL_ENABLE_EXTENSION_ALIAS,
    };

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "unity_alias_function_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));

    PFN_xrVoidFunction function = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(instance, UNITY_METAL_GRAPHICS_REQUIREMENTS_FUNCTION_ALIAS, &function));
    REQUIRE(function != nullptr);

    auto getMetalGraphicsRequirementsKHRX2 =
        reinterpret_cast<PFN_xrGetMetalGraphicsRequirementsKHR>(function);

    XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance, &systemGetInfo, &systemId));

    XrGraphicsRequirementsMetalKHR graphicsRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
    XR_CHECK(getMetalGraphicsRequirementsKHRX2(instance, systemId, &graphicsRequirements));

    xrDestroyInstance(instance);
}

TEST_CASE("Runtime requires a non-null Metal command queue", "[runtime][loader]")
{
    std::array<const char*, 1> enabledExtensions = {
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
    };

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "metal_null_queue_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));

    auto getMetalGraphicsRequirementsKHR =
        reinterpret_cast<PFN_xrGetMetalGraphicsRequirementsKHR>(
            GetProc(instance, "xrGetMetalGraphicsRequirementsKHR"));

    XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance, &systemGetInfo, &systemId));

    XrGraphicsRequirementsMetalKHR graphicsRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
    XR_CHECK(getMetalGraphicsRequirementsKHR(instance, systemId, &graphicsRequirements));

    XrGraphicsBindingMetalKHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
    graphicsBinding.commandQueue = nullptr;

    XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &graphicsBinding;
    sessionCreateInfo.systemId = systemId;

    XrSession session = XR_NULL_HANDLE;
    CHECK(xrCreateSession(instance, &sessionCreateInfo, &session) == XR_ERROR_VALIDATION_FAILURE);
    CHECK(session == XR_NULL_HANDLE);

    XR_CHECK(xrDestroyInstance(instance));
}

TEST_CASE("Runtime requires Metal graphics requirements before session creation", "[runtime][loader]")
{
    std::array<const char*, 1> enabledExtensions = {
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
    };

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "metal_requirements_before_session_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));

    XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance, &systemGetInfo, &systemId));

    XrGraphicsBindingMetalKHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
    graphicsBinding.commandQueue = DummyMetalCommandQueue();

    XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &graphicsBinding;
    sessionCreateInfo.systemId = systemId;

    XrSession session = XR_NULL_HANDLE;
    CHECK(xrCreateSession(instance, &sessionCreateInfo, &session) ==
          XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);
    CHECK(session == XR_NULL_HANDLE);

    XR_CHECK(xrDestroyInstance(instance));
}

TEST_CASE("Runtime validates the session system id before graphics checks", "[runtime][loader]")
{
    std::array<const char*, 1> enabledExtensions = {
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
    };

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "session_system_id_validation_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));

    auto getMetalGraphicsRequirementsKHR =
        reinterpret_cast<PFN_xrGetMetalGraphicsRequirementsKHR>(
            GetProc(instance, "xrGetMetalGraphicsRequirementsKHR"));

    XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance, &systemGetInfo, &systemId));

    XrGraphicsRequirementsMetalKHR graphicsRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
    XR_CHECK(getMetalGraphicsRequirementsKHR(instance, systemId, &graphicsRequirements));

    XrGraphicsBindingMetalKHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
    graphicsBinding.commandQueue = DummyMetalCommandQueue();

    XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &graphicsBinding;

    XrSession session = XR_NULL_HANDLE;

    sessionCreateInfo.systemId = XR_NULL_SYSTEM_ID;
    CHECK(xrCreateSession(instance, &sessionCreateInfo, &session) == XR_ERROR_SYSTEM_INVALID);
    CHECK(session == XR_NULL_HANDLE);

    sessionCreateInfo.systemId = systemId + 1;
    CHECK(xrCreateSession(instance, &sessionCreateInfo, &session) == XR_ERROR_SYSTEM_INVALID);
    CHECK(session == XR_NULL_HANDLE);

    XR_CHECK(xrDestroyInstance(instance));
}

TEST_CASE("Runtime validates extension gating and path formatting", "[runtime][loader]")
{
    RuntimeSessionContext context({XR_KHR_METAL_ENABLE_EXTENSION_NAME});

    PFN_xrVoidFunction function = nullptr;
    CHECK(xrGetInstanceProcAddr(context.instance, "xrSetInputDeviceActiveEXT", &function) ==
          XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(function == nullptr);
    CHECK(xrGetInstanceProcAddr(context.instance, "xrCreateDebugUtilsMessengerEXT", &function) ==
          XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(function == nullptr);

    XrPath invalidPath = XR_NULL_PATH;
    CHECK(xrStringToPath(context.instance, "invalid_path", &invalidPath) ==
          XR_ERROR_PATH_FORMAT_INVALID);
    CHECK(xrStringToPath(context.instance, "/", &invalidPath) ==
          XR_ERROR_PATH_FORMAT_INVALID);
    CHECK(xrStringToPath(context.instance, "/../foo", &invalidPath) ==
          XR_ERROR_PATH_FORMAT_INVALID);
    CHECK(xrStringToPath(context.instance, "/.", &invalidPath) ==
          XR_ERROR_PATH_FORMAT_INVALID);

    XrSystemHandTrackingPropertiesEXT handTrackingProperties = {
        XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties systemProperties = {XR_TYPE_SYSTEM_PROPERTIES};
    systemProperties.next = &handTrackingProperties;
    XR_CHECK(xrGetSystemProperties(context.instance, context.systemId, &systemProperties));
    CHECK(handTrackingProperties.supportsHandTracking == XR_FALSE);
}

TEST_CASE("Runtime gates xrLocateSpaces behind OpenXR 1.1", "[runtime][loader]")
{
    const char* enabledExtensions[] = {XR_KHR_METAL_ENABLE_EXTENSION_NAME};

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "oxrsys_runtime_api_version_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 57);
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = enabledExtensions;

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));

    PFN_xrVoidFunction function = nullptr;
    CHECK(xrGetInstanceProcAddr(instance, "xrLocateSpaces", &function) ==
          XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(function == nullptr);

    CHECK(xrGetInstanceProcAddr(instance, XR_LOCATE_SPACES_KHR_FUNCTION_ALIAS, &function) ==
          XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(function == nullptr);

    XR_CHECK(xrDestroyInstance(instance));
}

TEST_CASE("Runtime validates xrCreateInstance api version flags names and api layers", "[runtime][loader]")
{
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "create_instance_validation_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    std::strncpy(createInfo.applicationInfo.engineName, "runtime_tests", XR_MAX_ENGINE_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));
    XR_CHECK(xrDestroyInstance(instance));

    createInfo.applicationInfo.apiVersion = 0;
    CHECK(xrCreateInstance(&createInfo, &instance) == XR_ERROR_API_VERSION_UNSUPPORTED);

    createInfo.applicationInfo.apiVersion = 1;
    CHECK(xrCreateInstance(&createInfo, &instance) == XR_ERROR_API_VERSION_UNSUPPORTED);

    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(XR_VERSION_MAJOR(XR_CURRENT_API_VERSION) + 1, 0, 0);
    CHECK(xrCreateInstance(&createInfo, &instance) == XR_ERROR_API_VERSION_UNSUPPORTED);

    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    CHECK(xrCreateInstance(&createInfo, &instance) == XR_ERROR_VALIDATION_FAILURE);

    createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "create_instance_validation_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    std::strncpy(createInfo.applicationInfo.engineName, "runtime_tests", XR_MAX_ENGINE_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.createFlags = 0x42;
    CHECK(xrCreateInstance(&createInfo, &instance) == XR_ERROR_VALIDATION_FAILURE);

    createInfo.createFlags = 0;
    const char* fakeLayer = "nonexistent_api_layer";
    createInfo.enabledApiLayerCount = 1;
    createInfo.enabledApiLayerNames = &fakeLayer;
    CHECK(xrCreateInstance(&createInfo, &instance) == XR_ERROR_API_LAYER_NOT_PRESENT);

    createInfo.enabledApiLayerCount = 0;
    createInfo.enabledApiLayerNames = nullptr;
    createInfo.applicationInfo.applicationName[0] = '\0';
    CHECK(xrCreateInstance(&createInfo, &instance) == XR_ERROR_NAME_INVALID);
}

TEST_CASE("Runtime exposes xrLocateSpaces KHR alias for OpenXR 1.1", "[runtime][loader]")
{
    RuntimeSessionContext context({XR_KHR_METAL_ENABLE_EXTENSION_NAME}, false);

    PFN_xrVoidFunction function = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(context.instance, "xrLocateSpaces", &function));
    REQUIRE(function != nullptr);

    function = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(context.instance, XR_LOCATE_SPACES_KHR_FUNCTION_ALIAS, &function));
    REQUIRE(function != nullptr);
}

TEST_CASE("Runtime enumerates and creates LOCAL_FLOOR reference spaces", "[runtime][spaces]")
{
    RuntimeSessionContext context({XR_KHR_METAL_ENABLE_EXTENSION_NAME});

    uint32_t spaceCount = 0;
    XR_CHECK(xrEnumerateReferenceSpaces(context.session, 0, &spaceCount, nullptr));
    REQUIRE(spaceCount >= 4);

    std::vector<XrReferenceSpaceType> spaces(spaceCount);
    XR_CHECK(xrEnumerateReferenceSpaces(context.session, spaceCount, &spaceCount, spaces.data()));
    CHECK(std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) != spaces.end());

    XrReferenceSpaceCreateInfo createInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
    createInfo.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};

    XrSpace localFloorSpace = XR_NULL_HANDLE;
    XR_CHECK(xrCreateReferenceSpace(context.session, &createInfo, &localFloorSpace));

    XrTime sampleTime = 1;
    XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
    XR_CHECK(xrLocateSpace(localFloorSpace, context.localSpace, sampleTime, &location));
    CHECK((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0);
    CHECK((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0);
    CHECK_THAT(location.pose.position.x, WithinAbs(0.0f, 0.001f));
    CHECK_THAT(location.pose.position.y, WithinAbs(0.0f, 0.001f));
    CHECK_THAT(location.pose.position.z, WithinAbs(0.0f, 0.001f));

    XR_CHECK(xrDestroySpace(localFloorSpace));
}

TEST_CASE("Runtime hides LOCAL_FLOOR for OpenXR 1.0 instances", "[runtime][spaces]")
{
    const char* enabledExtensions[] = {XR_KHR_METAL_ENABLE_EXTENSION_NAME};

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "oxrsys_runtime_local_floor_1_0_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 57);
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = enabledExtensions;

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));

    XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance, &systemGetInfo, &systemId));

    auto getMetalGraphicsRequirementsKHR =
        reinterpret_cast<PFN_xrGetMetalGraphicsRequirementsKHR>(
            GetProc(instance, "xrGetMetalGraphicsRequirementsKHR"));
    XrGraphicsRequirementsMetalKHR graphicsRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
    XR_CHECK(getMetalGraphicsRequirementsKHR(instance, systemId, &graphicsRequirements));

    XrGraphicsBindingMetalKHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
    graphicsBinding.commandQueue = DummyMetalCommandQueue();

    XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &graphicsBinding;
    sessionCreateInfo.systemId = systemId;

    XrSession session = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSession(instance, &sessionCreateInfo, &session));

    uint32_t spaceCount = 0;
    XR_CHECK(xrEnumerateReferenceSpaces(session, 0, &spaceCount, nullptr));
    std::vector<XrReferenceSpaceType> spaces(spaceCount);
    XR_CHECK(xrEnumerateReferenceSpaces(session, spaceCount, &spaceCount, spaces.data()));
    CHECK(std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) == spaces.end());

    XrReferenceSpaceCreateInfo referenceSpaceCreateInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
    referenceSpaceCreateInfo.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};

    XrSpace space = XR_NULL_HANDLE;
    CHECK(xrCreateReferenceSpace(session, &referenceSpaceCreateInfo, &space) ==
          XR_ERROR_REFERENCE_SPACE_UNSUPPORTED);
    CHECK(space == XR_NULL_HANDLE);

    XR_CHECK(xrDestroySession(session));
    XR_CHECK(xrDestroyInstance(instance));
}

TEST_CASE("Runtime validates xrLocateSpaces count and velocity invariants", "[runtime][spaces]")
{
    RuntimeSessionContext context({XR_KHR_METAL_ENABLE_EXTENSION_NAME});

    XrReferenceSpaceCreateInfo createInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    createInfo.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};

    std::array<XrSpace, 2> spaces = {};
    XR_CHECK(xrCreateReferenceSpace(context.session, &createInfo, &spaces[0]));
    XR_CHECK(xrCreateReferenceSpace(context.session, &createInfo, &spaces[1]));

    std::array<XrSpaceLocationData, 2> locationData = {};
    XrSpaceLocations spaceLocations = {XR_TYPE_SPACE_LOCATIONS};
    spaceLocations.locationCount = static_cast<uint32_t>(locationData.size());
    spaceLocations.locations = locationData.data();

    XrSpacesLocateInfo locateInfo = {XR_TYPE_SPACES_LOCATE_INFO};
    locateInfo.baseSpace = context.localSpace;
    locateInfo.time = 1;
    locateInfo.spaceCount = 0;
    locateInfo.spaces = spaces.data();

    CHECK(xrLocateSpaces(context.session, &locateInfo, &spaceLocations) == XR_ERROR_VALIDATION_FAILURE);

    locateInfo.spaceCount = static_cast<uint32_t>(spaces.size());
    spaceLocations.locationCount = static_cast<uint32_t>(locationData.size()) - 1;
    CHECK(xrLocateSpaces(context.session, &locateInfo, &spaceLocations) == XR_ERROR_VALIDATION_FAILURE);

    spaceLocations.locationCount = static_cast<uint32_t>(locationData.size());
    XrSpaceVelocities velocities = {XR_TYPE_SPACE_VELOCITIES};
    std::array<XrSpaceVelocityData, 2> velocityData = {};
    velocities.velocityCount = static_cast<uint32_t>(velocityData.size()) - 1;
    velocities.velocities = velocityData.data();
    spaceLocations.next = &velocities;

    CHECK(xrLocateSpaces(context.session, &locateInfo, &spaceLocations) == XR_ERROR_VALIDATION_FAILURE);

    velocities.velocityCount = static_cast<uint32_t>(velocityData.size());
    XR_CHECK(xrLocateSpaces(context.session, &locateInfo, &spaceLocations));
    CHECK((velocityData[0].velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0);
    CHECK((velocityData[0].velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0);
    CHECK((velocityData[1].velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0);
    CHECK((velocityData[1].velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0);

    XR_CHECK(xrDestroySpace(spaces[1]));
    XR_CHECK(xrDestroySpace(spaces[0]));
}

TEST_CASE("Runtime stringifies OpenXR result and structure enums", "[runtime][loader]")
{
    RuntimeSessionContext context({XR_KHR_METAL_ENABLE_EXTENSION_NAME});

    char resultBuffer[XR_MAX_RESULT_STRING_SIZE] = {};
    char structureBuffer[XR_MAX_STRUCTURE_NAME_SIZE] = {};

    XR_CHECK(xrResultToString(context.instance, XR_ERROR_HANDLE_INVALID, resultBuffer));
    CHECK(std::string(resultBuffer) == "XR_ERROR_HANDLE_INVALID");

    XR_CHECK(xrResultToString(context.instance, static_cast<XrResult>(0x7ffffffe), resultBuffer));
    CHECK(std::string(resultBuffer) == "XR_UNKNOWN_SUCCESS_2147483646");

    XR_CHECK(xrStructureTypeToString(context.instance, XR_TYPE_FRAME_END_INFO, structureBuffer));
    CHECK(std::string(structureBuffer) == "XR_TYPE_FRAME_END_INFO");

    XR_CHECK(xrStructureTypeToString(context.instance, static_cast<XrStructureType>(0x7ffffffe), structureBuffer));
    CHECK(std::string(structureBuffer) == "XR_UNKNOWN_STRUCTURE_TYPE_2147483646");
}

TEST_CASE("Debug utils messenger callbacks work from instance create and explicit creation", "[runtime][loader]")
{
    uint32_t callbackCount = 0;

    XrDebugUtilsMessengerCreateInfoEXT chainedCreateInfo = {XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    chainedCreateInfo.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    chainedCreateInfo.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    chainedCreateInfo.userCallback = CountingDebugUtilsCallback;
    chainedCreateInfo.userData = &callbackCount;

    std::array<const char*, 2> enabledExtensions = {
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &chainedCreateInfo;
    std::strncpy(createInfo.applicationInfo.applicationName, "oxrsys_runtime_debug_utils_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));

    auto createMessenger = reinterpret_cast<PFN_xrCreateDebugUtilsMessengerEXT>(
        GetProc(instance, "xrCreateDebugUtilsMessengerEXT"));
    auto destroyMessenger = reinterpret_cast<PFN_xrDestroyDebugUtilsMessengerEXT>(
        GetProc(instance, "xrDestroyDebugUtilsMessengerEXT"));
    auto submitMessage = reinterpret_cast<PFN_xrSubmitDebugUtilsMessageEXT>(
        GetProc(instance, "xrSubmitDebugUtilsMessageEXT"));
    auto setObjectName = reinterpret_cast<PFN_xrSetDebugUtilsObjectNameEXT>(
        GetProc(instance, "xrSetDebugUtilsObjectNameEXT"));

    XrDebugUtilsMessengerEXT messenger = XR_NULL_HANDLE;
    XR_CHECK(createMessenger(instance, &chainedCreateInfo, &messenger));

    XrDebugUtilsObjectNameInfoEXT objectNameInfo = {XR_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    objectNameInfo.objectType = XR_OBJECT_TYPE_INSTANCE;
    objectNameInfo.objectHandle = reinterpret_cast<uint64_t>(instance);
    objectNameInfo.objectName = "runtime-test-instance";
    XR_CHECK(setObjectName(instance, &objectNameInfo));

    XrDebugUtilsMessengerCallbackDataEXT callbackData = {XR_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    callbackData.messageId = "runtime-test";
    callbackData.functionName = "xrSubmitDebugUtilsMessageEXT";
    callbackData.message = "Debug utils callback";
    XR_CHECK(submitMessage(instance,
                           XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                           XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
                           &callbackData));
    CHECK(callbackCount == 2);

    XR_CHECK(destroyMessenger(messenger));
    XR_CHECK(xrDestroyInstance(instance));
}

TEST_CASE("Debug utils resolves object names and session labels", "[runtime][loader]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_DEBUG_UTILS_EXTENSION_NAME,
    });

    auto createMessenger = reinterpret_cast<PFN_xrCreateDebugUtilsMessengerEXT>(
        GetProc(context.instance, "xrCreateDebugUtilsMessengerEXT"));
    auto destroyMessenger = reinterpret_cast<PFN_xrDestroyDebugUtilsMessengerEXT>(
        GetProc(context.instance, "xrDestroyDebugUtilsMessengerEXT"));
    auto submitMessage = reinterpret_cast<PFN_xrSubmitDebugUtilsMessageEXT>(
        GetProc(context.instance, "xrSubmitDebugUtilsMessageEXT"));
    auto setObjectName = reinterpret_cast<PFN_xrSetDebugUtilsObjectNameEXT>(
        GetProc(context.instance, "xrSetDebugUtilsObjectNameEXT"));
    auto beginLabelRegion = reinterpret_cast<PFN_xrSessionBeginDebugUtilsLabelRegionEXT>(
        GetProc(context.instance, "xrSessionBeginDebugUtilsLabelRegionEXT"));
    auto endLabelRegion = reinterpret_cast<PFN_xrSessionEndDebugUtilsLabelRegionEXT>(
        GetProc(context.instance, "xrSessionEndDebugUtilsLabelRegionEXT"));
    auto insertLabel = reinterpret_cast<PFN_xrSessionInsertDebugUtilsLabelEXT>(
        GetProc(context.instance, "xrSessionInsertDebugUtilsLabelEXT"));

    CapturedDebugUtilsMessage capturedMessage;

    XrDebugUtilsMessengerCreateInfoEXT createInfo = {XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    createInfo.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
    createInfo.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.userCallback = CaptureDebugUtilsCallback;
    createInfo.userData = &capturedMessage;

    XrDebugUtilsMessengerEXT messenger = XR_NULL_HANDLE;
    XR_CHECK(createMessenger(context.instance, &createInfo, &messenger));

    XrDebugUtilsObjectNameInfoEXT instanceNameInfo = {XR_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    instanceNameInfo.objectType = XR_OBJECT_TYPE_INSTANCE;
    instanceNameInfo.objectHandle = reinterpret_cast<uint64_t>(context.instance);
    instanceNameInfo.objectName = "My Instance Obj";
    XR_CHECK(setObjectName(context.instance, &instanceNameInfo));

    XrDebugUtilsLabelEXT label = {XR_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.labelName = "First individual label";
    XR_CHECK(insertLabel(context.session, &label));

    std::array<XrDebugUtilsObjectNameInfoEXT, 2> objects = {{
        {XR_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT},
        {XR_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT},
    }};
    objects[0].objectType = XR_OBJECT_TYPE_INSTANCE;
    objects[0].objectHandle = reinterpret_cast<uint64_t>(context.instance);
    objects[0].objectName = "Not my instance";
    objects[1].objectType = XR_OBJECT_TYPE_SESSION;
    objects[1].objectHandle = reinterpret_cast<uint64_t>(context.session);
    objects[1].objectName = "My Session Obj";

    XrDebugUtilsMessengerCallbackDataEXT callbackData = {XR_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    callbackData.messageId = "debug-utils-first";
    callbackData.functionName = "xrSubmitDebugUtilsMessageEXT";
    callbackData.message = "Debug utils session label test";
    callbackData.objectCount = static_cast<uint32_t>(objects.size());
    callbackData.objects = objects.data();

    XR_CHECK(submitMessage(context.instance,
                           XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
                           XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                           &callbackData));
    REQUIRE(capturedMessage.objectNames.size() == 2);
    CHECK(capturedMessage.objectNames[0] == "My Instance Obj");
    CHECK(capturedMessage.objectNames[1] == "My Session Obj");
    REQUIRE(capturedMessage.sessionLabels.size() == 1);
    CHECK(capturedMessage.sessionLabels[0] == "First individual label");

    label.labelName = "Region label";
    XR_CHECK(beginLabelRegion(context.session, &label));

    callbackData.messageId = "debug-utils-region";
    XR_CHECK(submitMessage(context.instance,
                           XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
                           XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                           &callbackData));
    REQUIRE(capturedMessage.sessionLabels.size() == 1);
    CHECK(capturedMessage.sessionLabels[0] == "Region label");

    XR_CHECK(endLabelRegion(context.session));
    callbackData.messageId = "debug-utils-none";
    XR_CHECK(submitMessage(context.instance,
                           XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
                           XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                           &callbackData));
    CHECK(capturedMessage.sessionLabels.empty());

    XR_CHECK(destroyMessenger(messenger));
}

TEST_CASE("BeginSession rejects unsupported view configuration types", "[runtime][loader]")
{
    RuntimeSessionContext context({XR_KHR_METAL_ENABLE_EXTENSION_NAME}, false);

    XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
    CHECK(xrBeginSession(context.session, &beginInfo) == XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
}

TEST_CASE("DestroySession tolerates active frame loop resources", "[runtime][loader]")
{
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "oxrsys_runtime_destroy_session_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    const char* enabledExtensions[] = {XR_KHR_METAL_ENABLE_EXTENSION_NAME};
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = enabledExtensions;

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));

    auto getMetalGraphicsRequirementsKHR =
        reinterpret_cast<PFN_xrGetMetalGraphicsRequirementsKHR>(
            GetProc(instance, "xrGetMetalGraphicsRequirementsKHR"));

    XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance, &systemGetInfo, &systemId));

    XrGraphicsRequirementsMetalKHR graphicsRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
    XR_CHECK(getMetalGraphicsRequirementsKHR(instance, systemId, &graphicsRequirements));

    XrGraphicsBindingMetalKHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
    graphicsBinding.commandQueue = DummyMetalCommandQueue();

    XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &graphicsBinding;
    sessionCreateInfo.systemId = systemId;

    XrSession session = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSession(instance, &sessionCreateInfo, &session));

    XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XR_CHECK(xrBeginSession(session, &beginInfo));

    int64_t format = SelectColorSwapchainFormat(session);
    XrSwapchain swapchain = CreateColorSwapchain(session, format);
    REQUIRE(swapchain != XR_NULL_HANDLE);

    XrReferenceSpaceCreateInfo spaceCreateInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceCreateInfo.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};

    XrSpace space = XR_NULL_HANDLE;
    XR_CHECK(xrCreateReferenceSpace(session, &spaceCreateInfo, &space));
    REQUIRE(space != XR_NULL_HANDLE);

    XR_CHECK(xrDestroySession(session));
    XR_CHECK(xrDestroyInstance(instance));
}

TEST_CASE("DestroyInstance tolerates active session resources", "[runtime][loader]")
{
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "oxrsys_runtime_destroy_instance_test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    const char* enabledExtensions[] = {XR_KHR_METAL_ENABLE_EXTENSION_NAME};
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = enabledExtensions;

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));

    auto getMetalGraphicsRequirementsKHR =
        reinterpret_cast<PFN_xrGetMetalGraphicsRequirementsKHR>(
            GetProc(instance, "xrGetMetalGraphicsRequirementsKHR"));

    XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance, &systemGetInfo, &systemId));

    XrGraphicsRequirementsMetalKHR graphicsRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
    XR_CHECK(getMetalGraphicsRequirementsKHR(instance, systemId, &graphicsRequirements));

    XrGraphicsBindingMetalKHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
    graphicsBinding.commandQueue = DummyMetalCommandQueue();

    XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &graphicsBinding;
    sessionCreateInfo.systemId = systemId;

    XrSession session = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSession(instance, &sessionCreateInfo, &session));

    XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XR_CHECK(xrBeginSession(session, &beginInfo));

    int64_t format = SelectColorSwapchainFormat(session);
    XrSwapchain swapchain = CreateColorSwapchain(session, format);
    REQUIRE(swapchain != XR_NULL_HANDLE);

    XR_CHECK(xrDestroyInstance(instance));
}

TEST_CASE("Conformance automation drives Khronos simple controller actions", "[runtime][actions]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME,
    });

    XrPath leftHandPath = context.Path("/user/hand/left");
    XrPath simpleControllerPath = context.Path("/interaction_profiles/khr/simple_controller");
    XrPath selectClickPath = context.Path("/user/hand/left/input/select/click");

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrActionSetCreateInfo actionSetCreateInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetCreateInfo.actionSetName, "simple", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo.localizedActionSetName, "Simple", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XR_CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &actionSet));

    XrAction selectAction = XR_NULL_HANDLE;
    XrActionCreateInfo actionCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    actionCreateInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    actionCreateInfo.countSubactionPaths = 1;
    actionCreateInfo.subactionPaths = &leftHandPath;
    std::strncpy(actionCreateInfo.actionName, "select", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(actionCreateInfo.localizedActionName, "Select", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &actionCreateInfo, &selectAction));

    XrActionSuggestedBinding suggestedBinding = {selectAction, selectClickPath};
    XrInteractionProfileSuggestedBinding suggestedBindings = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBindings.interactionProfile = simpleControllerPath;
    suggestedBindings.suggestedBindings = &suggestedBinding;
    suggestedBindings.countSuggestedBindings = 1;
    XR_CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings));

    XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet;
    XR_CHECK(xrAttachSessionActionSets(context.session, &attachInfo));

    XR_CHECK(context.setInputDeviceActiveEXT(context.session, simpleControllerPath, leftHandPath, XR_TRUE));
    XR_CHECK(context.setInputDeviceStateBoolEXT(context.session, leftHandPath,
                                                context.Path("/input/select/click"), XR_TRUE));

    XrActiveActionSet activeActionSet = {};
    activeActionSet.actionSet = actionSet;
    XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    XR_CHECK(xrSyncActions(context.session, &syncInfo));

    XrInteractionProfileState interactionProfileState = {XR_TYPE_INTERACTION_PROFILE_STATE};
    XR_CHECK(xrGetCurrentInteractionProfile(context.session, leftHandPath, &interactionProfileState));
    CHECK(interactionProfileState.interactionProfile == simpleControllerPath);

    XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = selectAction;
    getInfo.subactionPath = leftHandPath;
    XrActionStateBoolean boolState = {XR_TYPE_ACTION_STATE_BOOLEAN};
    XR_CHECK(xrGetActionStateBoolean(context.session, &getInfo, &boolState));
    CHECK(boolState.isActive == XR_TRUE);
    CHECK(boolState.currentState == XR_TRUE);

    uint32_t sourceCount = 0;
    XrBoundSourcesForActionEnumerateInfo enumerateInfo = {
        XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
    enumerateInfo.action = selectAction;
    XR_CHECK(xrEnumerateBoundSourcesForAction(context.session, &enumerateInfo, 0, &sourceCount, nullptr));
    REQUIRE(sourceCount == 1);

    std::array<XrPath, 1> sources = {};
    XR_CHECK(xrEnumerateBoundSourcesForAction(context.session, &enumerateInfo,
                                              static_cast<uint32_t>(sources.size()), &sourceCount,
                                              sources.data()));
    CHECK(sources[0] == selectClickPath);

    char localizedName[XR_MAX_PATH_LENGTH] = {};
    uint32_t localizedNameCount = 0;
    XrInputSourceLocalizedNameGetInfo localizedNameInfo = {
        XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO};
    localizedNameInfo.sourcePath = selectClickPath;
    localizedNameInfo.whichComponents =
        XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT | XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;
    XR_CHECK(xrGetInputSourceLocalizedName(context.session, &localizedNameInfo,
                                           sizeof(localizedName), &localizedNameCount, localizedName));
    CHECK(std::string(localizedName).find("Left Hand") != std::string::npos);
    CHECK(std::string(localizedName).find("Select") != std::string::npos);

    XR_CHECK(xrDestroyAction(selectAction));
    XR_CHECK(xrDestroyActionSet(actionSet));
}

TEST_CASE("Action state queries validate attachment subaction paths and action types", "[runtime][actions]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME,
    });

    XrPath leftHandPath = context.Path("/user/hand/left");
    XrPath selectClickPath = context.Path("/user/hand/left/input/select/click");

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrActionSetCreateInfo actionSetCreateInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetCreateInfo.actionSetName, "validate", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo.localizedActionSetName, "Validate", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XR_CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &actionSet));

    XrAction booleanAction = XR_NULL_HANDLE;
    XrActionCreateInfo booleanCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    booleanCreateInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    booleanCreateInfo.countSubactionPaths = 1;
    booleanCreateInfo.subactionPaths = &leftHandPath;
    std::strncpy(booleanCreateInfo.actionName, "boolean_action", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(booleanCreateInfo.localizedActionName, "Boolean Action", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &booleanCreateInfo, &booleanAction));

    XrAction hapticAction = XR_NULL_HANDLE;
    XrActionCreateInfo hapticCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    hapticCreateInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    hapticCreateInfo.countSubactionPaths = 1;
    hapticCreateInfo.subactionPaths = &leftHandPath;
    std::strncpy(hapticCreateInfo.actionName, "haptic_action", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(hapticCreateInfo.localizedActionName, "Haptic Action", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &hapticCreateInfo, &hapticAction));

    XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = booleanAction;
    getInfo.subactionPath = leftHandPath;

    XrActionStateBoolean booleanState = {XR_TYPE_ACTION_STATE_BOOLEAN};
    CHECK(xrGetActionStateBoolean(context.session, &getInfo, &booleanState) ==
          XR_ERROR_ACTIONSET_NOT_ATTACHED);

    XrInteractionProfileState interactionProfileState = {XR_TYPE_INTERACTION_PROFILE_STATE};
    CHECK(xrGetCurrentInteractionProfile(context.session, leftHandPath, &interactionProfileState) ==
          XR_ERROR_ACTIONSET_NOT_ATTACHED);

    uint32_t sourceCount = 0;
    XrBoundSourcesForActionEnumerateInfo enumerateInfo = {
        XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
    enumerateInfo.action = booleanAction;
    CHECK(xrEnumerateBoundSourcesForAction(context.session, &enumerateInfo, 0, &sourceCount, nullptr) ==
          XR_ERROR_ACTIONSET_NOT_ATTACHED);

    XrHapticActionInfo hapticActionInfo = {XR_TYPE_HAPTIC_ACTION_INFO};
    hapticActionInfo.action = hapticAction;
    hapticActionInfo.subactionPath = leftHandPath;
    XrHapticVibration vibration = {XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = 1.0f;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    vibration.duration = XR_MIN_HAPTIC_DURATION;
    CHECK(xrApplyHapticFeedback(context.session, &hapticActionInfo,
                                reinterpret_cast<XrHapticBaseHeader*>(&vibration)) ==
          XR_ERROR_ACTIONSET_NOT_ATTACHED);
    CHECK(xrStopHapticFeedback(context.session, &hapticActionInfo) ==
          XR_ERROR_ACTIONSET_NOT_ATTACHED);

    XrSessionActionSetsAttachInfo invalidAttachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    invalidAttachInfo.countActionSets = 0;
    CHECK(xrAttachSessionActionSets(context.session, &invalidAttachInfo) ==
          XR_ERROR_VALIDATION_FAILURE);

    XrActionSuggestedBinding binding = {booleanAction, selectClickPath};
    XrInteractionProfileSuggestedBinding suggestedBindings = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBindings.interactionProfile = context.Path("/interaction_profiles/khr/simple_controller");
    suggestedBindings.suggestedBindings = &binding;
    suggestedBindings.countSuggestedBindings = 1;
    XR_CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings));

    XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet;
    XR_CHECK(xrAttachSessionActionSets(context.session, &attachInfo));

    XrAction lateAction = XR_NULL_HANDLE;
    CHECK(xrCreateAction(actionSet, &booleanCreateInfo, &lateAction) ==
          XR_ERROR_ACTIONSETS_ALREADY_ATTACHED);

    getInfo.subactionPath = XR_NULL_PATH;
    XR_CHECK(xrGetActionStateBoolean(context.session, &getInfo, &booleanState));

    getInfo.subactionPath = context.Path("/user/gamepad");
    CHECK(xrGetActionStateBoolean(context.session, &getInfo, &booleanState) ==
          XR_ERROR_PATH_UNSUPPORTED);

    getInfo.subactionPath = static_cast<XrPath>(0x12345678);
    CHECK(xrGetActionStateBoolean(context.session, &getInfo, &booleanState) ==
          XR_ERROR_PATH_INVALID);

    getInfo.subactionPath = leftHandPath;
    XrActionStateFloat floatState = {XR_TYPE_ACTION_STATE_FLOAT};
    XrActionStateVector2f vectorState = {XR_TYPE_ACTION_STATE_VECTOR2F};
    XrActionStatePose poseState = {XR_TYPE_ACTION_STATE_POSE};
    CHECK(xrGetActionStateFloat(context.session, &getInfo, &floatState) ==
          XR_ERROR_ACTION_TYPE_MISMATCH);
    CHECK(xrGetActionStateVector2f(context.session, &getInfo, &vectorState) ==
          XR_ERROR_ACTION_TYPE_MISMATCH);
    CHECK(xrGetActionStatePose(context.session, &getInfo, &poseState) ==
          XR_ERROR_ACTION_TYPE_MISMATCH);
    CHECK((xrApplyHapticFeedback(context.session, &hapticActionInfo,
                                 reinterpret_cast<XrHapticBaseHeader*>(&vibration)) ==
           XR_SESSION_NOT_FOCUSED));
    CHECK(xrStopHapticFeedback(context.session, &hapticActionInfo) == XR_SESSION_NOT_FOCUSED);

    XR_CHECK(xrDestroyAction(booleanAction));
    XR_CHECK(xrDestroyAction(hapticAction));
    XR_CHECK(xrDestroyActionSet(actionSet));
}

TEST_CASE("Action creation validates names duplicates and subaction paths", "[runtime][actions]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
    }, false);

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrActionSetCreateInfo actionSetCreateInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetCreateInfo.actionSetName, "test_action_set", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo.localizedActionSetName, "Test Action Set",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XR_CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &actionSet));

    XrActionSet duplicateActionSet = XR_NULL_HANDLE;
    CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &duplicateActionSet) ==
          XR_ERROR_NAME_DUPLICATED);

    XrActionSet localizedDuplicateActionSet = XR_NULL_HANDLE;
    std::strncpy(actionSetCreateInfo.actionSetName, "test_action_set_two", XR_MAX_ACTION_SET_NAME_SIZE);
    CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &localizedDuplicateActionSet) ==
          XR_ERROR_LOCALIZED_NAME_DUPLICATED);

    std::strncpy(actionSetCreateInfo.actionSetName, "INVALID_NAME", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo.localizedActionSetName, "Invalid Name", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &duplicateActionSet) ==
          XR_ERROR_PATH_FORMAT_INVALID);

    XrAction action = XR_NULL_HANDLE;
    XrActionCreateInfo actionCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    actionCreateInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(actionCreateInfo.actionName, "test_action", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(actionCreateInfo.localizedActionName, "Test Action", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &actionCreateInfo, &action));

    XrAction duplicateAction = XR_NULL_HANDLE;
    CHECK(xrCreateAction(actionSet, &actionCreateInfo, &duplicateAction) == XR_ERROR_NAME_DUPLICATED);

    XrAction localizedDuplicateAction = XR_NULL_HANDLE;
    std::strncpy(actionCreateInfo.actionName, "test_action_two", XR_MAX_ACTION_NAME_SIZE);
    CHECK(xrCreateAction(actionSet, &actionCreateInfo, &localizedDuplicateAction) ==
          XR_ERROR_LOCALIZED_NAME_DUPLICATED);

    std::strncpy(actionCreateInfo.actionName, "invalid name", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(actionCreateInfo.localizedActionName, "Invalid Action", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    CHECK(xrCreateAction(actionSet, &actionCreateInfo, &duplicateAction) ==
          XR_ERROR_PATH_FORMAT_INVALID);

    XrPath invalidSubactionPath = context.Path("/user/head");
    actionCreateInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    std::strncpy(actionCreateInfo.actionName, "float_action", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(actionCreateInfo.localizedActionName, "Float Action", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    actionCreateInfo.countSubactionPaths = 1;
    actionCreateInfo.subactionPaths = &invalidSubactionPath;
    CHECK(xrCreateAction(actionSet, &actionCreateInfo, &duplicateAction) == XR_ERROR_PATH_UNSUPPORTED);

    XrPath duplicateSubactionPaths[2] = {context.Path("/user/hand/left"), context.Path("/user/hand/left")};
    actionCreateInfo.countSubactionPaths = 2;
    actionCreateInfo.subactionPaths = duplicateSubactionPaths;
    CHECK(xrCreateAction(actionSet, &actionCreateInfo, &duplicateAction) == XR_ERROR_PATH_UNSUPPORTED);

    XR_CHECK(xrDestroyAction(action));
    XR_CHECK(xrDestroyActionSet(actionSet));
}

TEST_CASE("Suggest interaction profile bindings validates known profiles and paths", "[runtime][actions]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
    });

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrActionSetCreateInfo actionSetCreateInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetCreateInfo.actionSetName, "bindings", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo.localizedActionSetName, "Bindings", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XR_CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &actionSet));

    XrAction action = XR_NULL_HANDLE;
    XrActionCreateInfo actionCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    actionCreateInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(actionCreateInfo.actionName, "select", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(actionCreateInfo.localizedActionName, "Select", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &actionCreateInfo, &action));

    XrActionSuggestedBinding binding = {action, context.Path("/user/hand/left/input/select/click")};
    XrInteractionProfileSuggestedBinding suggestedBindings = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBindings.suggestedBindings = &binding;
    suggestedBindings.countSuggestedBindings = 1;

    suggestedBindings.interactionProfile = context.Path("/interaction_profiles/khr/simple_controller");
    XR_CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings));

    suggestedBindings.countSuggestedBindings = 0;
    CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings) ==
          XR_ERROR_VALIDATION_FAILURE);

    suggestedBindings.countSuggestedBindings = 1;
    suggestedBindings.interactionProfile = context.Path("/interaction_profiles/khr/another_controller");
    CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings) ==
          XR_ERROR_PATH_UNSUPPORTED);

    suggestedBindings.interactionProfile = context.Path("/interaction_profiles/khr/simple_controller");
    binding.binding = context.Path("/user/hand/right");
    CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings) ==
          XR_ERROR_PATH_UNSUPPORTED);

    binding.binding = context.Path("/user/hand/left/input/select/click");
    XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet;
    XR_CHECK(xrAttachSessionActionSets(context.session, &attachInfo));
    CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings) ==
          XR_ERROR_ACTIONSETS_ALREADY_ATTACHED);

    XR_CHECK(xrDestroyAction(action));
    XR_CHECK(xrDestroyActionSet(actionSet));
}

TEST_CASE("Suggest interaction profile bindings rejects unavailable extension-gated components",
          "[runtime][actions]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
    });

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrActionSetCreateInfo actionSetCreateInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetCreateInfo.actionSetName, "gated_bindings", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo.localizedActionSetName, "Gated Bindings",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XR_CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &actionSet));

    XrAction poseAction = XR_NULL_HANDLE;
    XrActionCreateInfo actionCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    actionCreateInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strncpy(actionCreateInfo.actionName, "pose_action", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(actionCreateInfo.localizedActionName, "Pose Action", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &actionCreateInfo, &poseAction));

    XrActionSuggestedBinding binding = {poseAction, context.Path("/user/hand/left/input/pinch_ext/pose")};
    XrInteractionProfileSuggestedBinding suggestedBindings = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBindings.interactionProfile = context.Path("/interaction_profiles/khr/simple_controller");
    suggestedBindings.suggestedBindings = &binding;
    suggestedBindings.countSuggestedBindings = 1;
    CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings) ==
          XR_ERROR_PATH_UNSUPPORTED);

    binding.binding = context.Path("/user/hand/left/input/trackpad/dpad_up");
    CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings) ==
          XR_ERROR_PATH_UNSUPPORTED);

    XR_CHECK(xrDestroyAction(poseAction));
    XR_CHECK(xrDestroyActionSet(actionSet));
}

TEST_CASE("Suggest interaction profile bindings accepts hand interaction components when enabled",
          "[runtime][actions]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_HAND_INTERACTION_EXTENSION_NAME,
    });

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrActionSetCreateInfo actionSetCreateInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetCreateInfo.actionSetName, "enabled_ext_bindings", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo.localizedActionSetName, "Enabled Ext Bindings",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XR_CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &actionSet));

    XrAction poseAction = XR_NULL_HANDLE;
    XrActionCreateInfo actionCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    actionCreateInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strncpy(actionCreateInfo.actionName, "ext_pose_action", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(actionCreateInfo.localizedActionName, "Ext Pose Action", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &actionCreateInfo, &poseAction));

    XrActionSuggestedBinding binding = {poseAction, context.Path("/user/hand/right/input/pinch_ext/pose")};
    XrInteractionProfileSuggestedBinding suggestedBindings = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBindings.interactionProfile = context.Path("/interaction_profiles/oculus/touch_controller");
    suggestedBindings.suggestedBindings = &binding;
    suggestedBindings.countSuggestedBindings = 1;
    XR_CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings));

    binding.binding = context.Path("/user/hand/right/input/poke_ext/pose");
    XR_CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings));

    XR_CHECK(xrDestroyAction(poseAction));
    XR_CHECK(xrDestroyActionSet(actionSet));
}

TEST_CASE("Suggest interaction profile bindings enforces availability-gated component paths",
          "[runtime][actions]")
{
    RuntimeSessionContext handContext({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_HAND_INTERACTION_EXTENSION_NAME,
    },
                                      false);

    XrActionSetCreateInfo actionSetCreateInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetCreateInfo.actionSetName, "availability_set", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo.localizedActionSetName, "Availability Set",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);

    XrActionSet actionSet = XR_NULL_HANDLE;
    XR_CHECK(xrCreateActionSet(handContext.instance, &actionSetCreateInfo, &actionSet));

    XrActionCreateInfo booleanActionCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    booleanActionCreateInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(booleanActionCreateInfo.actionName, "availability_bool", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(booleanActionCreateInfo.localizedActionName, "Availability Bool",
                 XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    XrAction booleanAction = XR_NULL_HANDLE;
    XR_CHECK(xrCreateAction(actionSet, &booleanActionCreateInfo, &booleanAction));

    XrActionCreateInfo floatActionCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    floatActionCreateInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    std::strncpy(floatActionCreateInfo.actionName, "availability_float", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(floatActionCreateInfo.localizedActionName, "Availability Float",
                 XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    XrAction floatAction = XR_NULL_HANDLE;
    XR_CHECK(xrCreateAction(actionSet, &floatActionCreateInfo, &floatAction));

    XrActionSuggestedBinding unsupportedBinding[] = {
        {booleanAction, handContext.Path("/user/hand/right/input/trigger/proximity_fb")},
    };
    XrInteractionProfileSuggestedBinding unsupportedSuggestInfo = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    unsupportedSuggestInfo.interactionProfile =
        handContext.Path("/interaction_profiles/oculus/touch_controller");
    unsupportedSuggestInfo.suggestedBindings = unsupportedBinding;
    unsupportedSuggestInfo.countSuggestedBindings = 1;
    REQUIRE(xrSuggestInteractionProfileBindings(handContext.instance, &unsupportedSuggestInfo) ==
            XR_ERROR_PATH_UNSUPPORTED);

    XrActionSuggestedBinding readyBindings[] = {
        {booleanAction, handContext.Path("/user/hand/left/input/pinch_ext/ready_ext")},
        {booleanAction, handContext.Path("/user/hand/left/input/grasp_ext/ready_ext")},
        {booleanAction, handContext.Path("/user/hand/left/input/aim_activate_ext/ready_ext")},
        {booleanAction, handContext.Path("/user/hand/right/input/pinch_ext/ready_ext")},
        {booleanAction, handContext.Path("/user/hand/right/input/grasp_ext/ready_ext")},
        {booleanAction, handContext.Path("/user/hand/right/input/aim_activate_ext/ready_ext")},
        {floatAction, handContext.Path("/user/hand/left/input/aim_activate_ext/value")},
        {floatAction, handContext.Path("/user/hand/right/input/aim_activate_ext/value")},
    };
    XrInteractionProfileSuggestedBinding readySuggestInfo = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    readySuggestInfo.interactionProfile =
        handContext.Path("/interaction_profiles/ext/hand_interaction_ext");
    readySuggestInfo.suggestedBindings = readyBindings;
    readySuggestInfo.countSuggestedBindings = std::size(readyBindings);
    XR_CHECK(xrSuggestInteractionProfileBindings(handContext.instance, &readySuggestInfo));

    XR_CHECK(xrDestroyAction(booleanAction));
    XR_CHECK(xrDestroyAction(floatAction));
    XR_CHECK(xrDestroyActionSet(actionSet));
}

TEST_CASE("Suggest interaction profile bindings accepts grip surface for hand interaction in 1.1",
          "[runtime][actions]")
{

    XrInstanceCreateInfo createInfo11 = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo11.applicationInfo.applicationName, "runtime_tests_hand_11",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo11.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 1, 0);
    const char* extensions11[] = {
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_HAND_INTERACTION_EXTENSION_NAME,
    };
    createInfo11.enabledExtensionCount = std::size(extensions11);
    createInfo11.enabledExtensionNames = extensions11;

    XrInstance instance11 = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo11, &instance11));

    XrActionSetCreateInfo actionSetCreateInfo11 = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetCreateInfo11.actionSetName, "availability11_set",
                 XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo11.localizedActionSetName, "Availability 11 Set",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);

    XrActionSet actionSet11 = XR_NULL_HANDLE;
    XR_CHECK(xrCreateActionSet(instance11, &actionSetCreateInfo11, &actionSet11));

    XrActionCreateInfo poseActionCreateInfo11 = {XR_TYPE_ACTION_CREATE_INFO};
    poseActionCreateInfo11.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strncpy(poseActionCreateInfo11.actionName, "availability11_pose", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(poseActionCreateInfo11.localizedActionName, "Availability 11 Pose",
                 XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    XrAction poseAction11 = XR_NULL_HANDLE;
    XR_CHECK(xrCreateAction(actionSet11, &poseActionCreateInfo11, &poseAction11));

    XrPath leftGripSurface11 = XR_NULL_PATH;
    XrPath rightGripSurface11 = XR_NULL_PATH;
    XrPath handProfile11 = XR_NULL_PATH;
    XR_CHECK(xrStringToPath(instance11, "/user/hand/left/input/grip_surface/pose", &leftGripSurface11));
    XR_CHECK(xrStringToPath(instance11, "/user/hand/right/input/grip_surface/pose", &rightGripSurface11));
    XR_CHECK(xrStringToPath(instance11, "/interaction_profiles/ext/hand_interaction_ext", &handProfile11));

    XrActionSuggestedBinding gripSurfaceBindings[] = {
        {poseAction11, leftGripSurface11},
        {poseAction11, rightGripSurface11},
    };
    XrInteractionProfileSuggestedBinding gripSurfaceSuggestInfo = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    gripSurfaceSuggestInfo.interactionProfile = handProfile11;
    gripSurfaceSuggestInfo.suggestedBindings = gripSurfaceBindings;
    gripSurfaceSuggestInfo.countSuggestedBindings = std::size(gripSurfaceBindings);
    XR_CHECK(xrSuggestInteractionProfileBindings(instance11, &gripSurfaceSuggestInfo));

    XR_CHECK(xrDestroyAction(poseAction11));
    XR_CHECK(xrDestroyActionSet(actionSet11));
    XR_CHECK(xrDestroyInstance(instance11));
}

TEST_CASE("Quest Touch profile reports float and vector inputs", "[runtime][actions]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME,
    });

    XrPath leftHandPath = context.Path("/user/hand/left");
    XrPath touchControllerPath = context.Path("/interaction_profiles/oculus/touch_controller");
    XrPath triggerValuePath = context.Path("/user/hand/left/input/trigger/value");
    XrPath thumbstickPath = context.Path("/user/hand/left/input/thumbstick");

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrActionSetCreateInfo actionSetCreateInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetCreateInfo.actionSetName, "touch", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo.localizedActionSetName, "Touch", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XR_CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &actionSet));

    XrAction triggerAction = XR_NULL_HANDLE;
    XrActionCreateInfo triggerCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    triggerCreateInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    triggerCreateInfo.countSubactionPaths = 1;
    triggerCreateInfo.subactionPaths = &leftHandPath;
    std::strncpy(triggerCreateInfo.actionName, "trigger", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(triggerCreateInfo.localizedActionName, "Trigger", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &triggerCreateInfo, &triggerAction));

    XrAction thumbstickAction = XR_NULL_HANDLE;
    XrActionCreateInfo thumbstickCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    thumbstickCreateInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    thumbstickCreateInfo.countSubactionPaths = 1;
    thumbstickCreateInfo.subactionPaths = &leftHandPath;
    std::strncpy(thumbstickCreateInfo.actionName, "thumbstick", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(thumbstickCreateInfo.localizedActionName, "Thumbstick", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &thumbstickCreateInfo, &thumbstickAction));

    std::array<XrActionSuggestedBinding, 2> bindings = {{
        {triggerAction, triggerValuePath},
        {thumbstickAction, thumbstickPath},
    }};
    XrInteractionProfileSuggestedBinding suggestedBindings = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBindings.interactionProfile = touchControllerPath;
    suggestedBindings.suggestedBindings = bindings.data();
    suggestedBindings.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    XR_CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings));

    XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet;
    XR_CHECK(xrAttachSessionActionSets(context.session, &attachInfo));

    XR_CHECK(context.setInputDeviceActiveEXT(context.session, touchControllerPath, leftHandPath, XR_TRUE));
    XR_CHECK(context.setInputDeviceStateFloatEXT(context.session, leftHandPath,
                                                 context.Path("/input/trigger/value"), 0.7f));
    XR_CHECK(context.setInputDeviceStateVector2fEXT(context.session, leftHandPath,
                                                    context.Path("/input/thumbstick"), {0.25f, -0.5f}));

    XrActiveActionSet activeActionSet = {};
    activeActionSet.actionSet = actionSet;
    XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    XR_CHECK(xrSyncActions(context.session, &syncInfo));

    XrInteractionProfileState interactionProfileState = {XR_TYPE_INTERACTION_PROFILE_STATE};
    XR_CHECK(xrGetCurrentInteractionProfile(context.session, leftHandPath, &interactionProfileState));
    CHECK(interactionProfileState.interactionProfile == touchControllerPath);

    XrActionStateGetInfo triggerGetInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    triggerGetInfo.action = triggerAction;
    triggerGetInfo.subactionPath = leftHandPath;
    XrActionStateFloat floatState = {XR_TYPE_ACTION_STATE_FLOAT};
    XR_CHECK(xrGetActionStateFloat(context.session, &triggerGetInfo, &floatState));
    CHECK(floatState.isActive == XR_TRUE);
    CHECK_THAT(floatState.currentState, WithinAbs(0.7f, 0.001f));

    XrActionStateGetInfo thumbstickGetInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    thumbstickGetInfo.action = thumbstickAction;
    thumbstickGetInfo.subactionPath = leftHandPath;
    XrActionStateVector2f vectorState = {XR_TYPE_ACTION_STATE_VECTOR2F};
    XR_CHECK(xrGetActionStateVector2f(context.session, &thumbstickGetInfo, &vectorState));
    CHECK(vectorState.isActive == XR_TRUE);
    CHECK_THAT(vectorState.currentState.x, WithinAbs(0.25f, 0.001f));
    CHECK_THAT(vectorState.currentState.y, WithinAbs(-0.5f, 0.001f));

    XR_CHECK(xrDestroyAction(triggerAction));
    XR_CHECK(xrDestroyAction(thumbstickAction));
    XR_CHECK(xrDestroyActionSet(actionSet));
}

TEST_CASE("Quest Pico and simple profiles report float inputs through automation", "[runtime][actions]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME,
    });

    XrPath leftHandPath = context.Path("/user/hand/left");

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrActionSetCreateInfo actionSetCreateInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetCreateInfo.actionSetName, "multi_controller",
                 XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo.localizedActionSetName, "Multi Controller",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XR_CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &actionSet));

    XrAction valueAction = XR_NULL_HANDLE;
    XrActionCreateInfo actionCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    actionCreateInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    actionCreateInfo.countSubactionPaths = 1;
    actionCreateInfo.subactionPaths = &leftHandPath;
    std::strncpy(actionCreateInfo.actionName, "controller_value", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(actionCreateInfo.localizedActionName, "Controller Value",
                 XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &actionCreateInfo, &valueAction));

    struct ProfileCase
    {
        const char* profilePath;
        const char* bindingPath;
        const char* statePath;
        float value;
    };

    const ProfileCase profiles[] = {
        {"/interaction_profiles/oculus/touch_controller",
         "/user/hand/left/input/trigger/value",
         "/input/trigger/value",
         0.20f},
        {"/interaction_profiles/meta/touch_controller_quest_2",
         "/user/hand/left/input/trigger/value",
         "/input/trigger/value",
         0.35f},
        {"/interaction_profiles/meta/touch_plus_controller",
         "/user/hand/left/input/trigger/value",
         "/input/trigger/value",
         0.50f},
        {"/interaction_profiles/bytedance/pico_neo3_controller",
         "/user/hand/left/input/trigger/value",
         "/input/trigger/value",
         0.65f},
        {"/interaction_profiles/bytedance/pico4_controller",
         "/user/hand/left/input/trigger/value",
         "/input/trigger/value",
         0.80f},
        {"/interaction_profiles/khr/simple_controller",
         "/user/hand/left/input/select/value",
         "/input/select/value",
         0.95f},
    };

    for (const ProfileCase& profile : profiles)
    {
        XrActionSuggestedBinding binding = {
            valueAction, context.Path(profile.bindingPath)};
        XrInteractionProfileSuggestedBinding suggestedBindings = {
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestedBindings.interactionProfile = context.Path(profile.profilePath);
        suggestedBindings.suggestedBindings = &binding;
        suggestedBindings.countSuggestedBindings = 1;
        XR_CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings));
    }

    XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet;
    XR_CHECK(xrAttachSessionActionSets(context.session, &attachInfo));

    XrActiveActionSet activeActionSet = {};
    activeActionSet.actionSet = actionSet;
    XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;

    for (const ProfileCase& profile : profiles)
    {
        INFO(profile.profilePath);
        XrPath interactionProfile = context.Path(profile.profilePath);
        XR_CHECK(context.setInputDeviceActiveEXT(context.session, interactionProfile,
                                                 leftHandPath, XR_TRUE));
        XR_CHECK(context.setInputDeviceStateFloatEXT(context.session, leftHandPath,
                                                     context.Path(profile.statePath),
                                                     profile.value));
        XR_CHECK(xrSyncActions(context.session, &syncInfo));

        XrInteractionProfileState interactionProfileState = {XR_TYPE_INTERACTION_PROFILE_STATE};
        XR_CHECK(xrGetCurrentInteractionProfile(context.session, leftHandPath,
                                                &interactionProfileState));
        CHECK(interactionProfileState.interactionProfile == interactionProfile);

        XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = valueAction;
        getInfo.subactionPath = leftHandPath;
        XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
        XR_CHECK(xrGetActionStateFloat(context.session, &getInfo, &state));
        CHECK(state.isActive == XR_TRUE);
        CHECK_THAT(state.currentState, WithinAbs(profile.value, 0.001f));
    }

    XR_CHECK(xrDestroyAction(valueAction));
    XR_CHECK(xrDestroyActionSet(actionSet));
}

TEST_CASE("Inactive action spaces clear location flags", "[runtime][actions]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME,
    });

    XrPath leftHandPath = context.Path("/user/hand/left");
    XrPath touchControllerPath = context.Path("/interaction_profiles/oculus/touch_controller");

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrActionSetCreateInfo actionSetCreateInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetCreateInfo.actionSetName, "inactive_pose", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo.localizedActionSetName, "Inactive Pose",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XR_CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &actionSet));

    XrAction poseAction = XR_NULL_HANDLE;
    XrActionCreateInfo poseCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    poseCreateInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    poseCreateInfo.countSubactionPaths = 1;
    poseCreateInfo.subactionPaths = &leftHandPath;
    std::strncpy(poseCreateInfo.actionName, "inactive_grip", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(poseCreateInfo.localizedActionName, "Inactive Grip",
                 XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &poseCreateInfo, &poseAction));

    XrActionSuggestedBinding binding = {
        poseAction, context.Path("/user/hand/left/input/grip/pose")};
    XrInteractionProfileSuggestedBinding suggestedBindings = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBindings.interactionProfile = touchControllerPath;
    suggestedBindings.suggestedBindings = &binding;
    suggestedBindings.countSuggestedBindings = 1;
    XR_CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings));

    XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet;
    XR_CHECK(xrAttachSessionActionSets(context.session, &attachInfo));

    XR_CHECK(context.setInputDeviceActiveEXT(context.session, touchControllerPath,
                                             leftHandPath, XR_FALSE));

    XrActiveActionSet activeActionSet = {};
    activeActionSet.actionSet = actionSet;
    XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    XR_CHECK(xrSyncActions(context.session, &syncInfo));

    XrActionStateGetInfo poseGetInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    poseGetInfo.action = poseAction;
    poseGetInfo.subactionPath = leftHandPath;
    XrActionStatePose poseState = {XR_TYPE_ACTION_STATE_POSE};
    XR_CHECK(xrGetActionStatePose(context.session, &poseGetInfo, &poseState));
    CHECK(poseState.isActive == XR_FALSE);

    XrSpace actionSpace = XR_NULL_HANDLE;
    XrActionSpaceCreateInfo actionSpaceCreateInfo = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    actionSpaceCreateInfo.action = poseAction;
    actionSpaceCreateInfo.subactionPath = leftHandPath;
    actionSpaceCreateInfo.poseInActionSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    XR_CHECK(xrCreateActionSpace(context.session, &actionSpaceCreateInfo, &actionSpace));

    XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
    XR_CHECK(xrLocateSpace(actionSpace, context.localSpace, 1, &location));
    CHECK(location.locationFlags == 0);

    XR_CHECK(xrDestroySpace(actionSpace));
    XR_CHECK(xrDestroyAction(poseAction));
    XR_CHECK(xrDestroyActionSet(actionSet));
}

TEST_CASE("Hand interaction pose and value inputs work through automation", "[runtime][hand]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME,
        XR_EXT_HAND_INTERACTION_EXTENSION_NAME,
    });

    XrPath leftHandPath = context.Path("/user/hand/left");
    XrPath handInteractionPath = context.Path("/interaction_profiles/ext/hand_interaction_ext");
    XrPath pinchPosePath = context.Path("/user/hand/left/input/pinch_ext/pose");
    XrPath pinchValuePath = context.Path("/user/hand/left/input/pinch_ext/value");

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrActionSetCreateInfo actionSetCreateInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetCreateInfo.actionSetName, "hands", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(actionSetCreateInfo.localizedActionSetName, "Hands", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XR_CHECK(xrCreateActionSet(context.instance, &actionSetCreateInfo, &actionSet));

    XrAction pinchPoseAction = XR_NULL_HANDLE;
    XrActionCreateInfo poseCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    poseCreateInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    poseCreateInfo.countSubactionPaths = 1;
    poseCreateInfo.subactionPaths = &leftHandPath;
    std::strncpy(poseCreateInfo.actionName, "pinch_pose", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(poseCreateInfo.localizedActionName, "Pinch Pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &poseCreateInfo, &pinchPoseAction));

    XrAction pinchValueAction = XR_NULL_HANDLE;
    XrActionCreateInfo valueCreateInfo = {XR_TYPE_ACTION_CREATE_INFO};
    valueCreateInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    valueCreateInfo.countSubactionPaths = 1;
    valueCreateInfo.subactionPaths = &leftHandPath;
    std::strncpy(valueCreateInfo.actionName, "pinch_value", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(valueCreateInfo.localizedActionName, "Pinch Value", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet, &valueCreateInfo, &pinchValueAction));

    std::array<XrActionSuggestedBinding, 2> bindings = {{
        {pinchPoseAction, pinchPosePath},
        {pinchValueAction, pinchValuePath},
    }};
    XrInteractionProfileSuggestedBinding suggestedBindings = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBindings.interactionProfile = handInteractionPath;
    suggestedBindings.suggestedBindings = bindings.data();
    suggestedBindings.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    XR_CHECK(xrSuggestInteractionProfileBindings(context.instance, &suggestedBindings));

    XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet;
    XR_CHECK(xrAttachSessionActionSets(context.session, &attachInfo));

    XR_CHECK(context.setInputDeviceActiveEXT(context.session, handInteractionPath, leftHandPath, XR_TRUE));

    XrPosef pinchPose = {};
    pinchPose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    pinchPose.position = {0.15f, 1.25f, -0.35f};
    XR_CHECK(context.setInputDeviceLocationEXT(context.session, leftHandPath,
                                               context.Path("/input/pinch_ext/pose"),
                                               context.localSpace, pinchPose));
    XR_CHECK(context.setInputDeviceStateFloatEXT(context.session, leftHandPath,
                                                 context.Path("/input/pinch_ext/value"), 0.8f));

    XrActiveActionSet activeActionSet = {};
    activeActionSet.actionSet = actionSet;
    XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    XR_CHECK(xrSyncActions(context.session, &syncInfo));

    XrActionStateGetInfo poseGetInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    poseGetInfo.action = pinchPoseAction;
    poseGetInfo.subactionPath = leftHandPath;
    XrActionStatePose poseState = {XR_TYPE_ACTION_STATE_POSE};
    XR_CHECK(xrGetActionStatePose(context.session, &poseGetInfo, &poseState));
    CHECK(poseState.isActive == XR_TRUE);

    XrSpace pinchActionSpace = XR_NULL_HANDLE;
    XrActionSpaceCreateInfo actionSpaceCreateInfo = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    actionSpaceCreateInfo.action = pinchPoseAction;
    actionSpaceCreateInfo.subactionPath = leftHandPath;
    actionSpaceCreateInfo.poseInActionSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    XR_CHECK(xrCreateActionSpace(context.session, &actionSpaceCreateInfo, &pinchActionSpace));

    XrTime sampleTime = 1;

    XrSpaceLocation spaceLocation = {XR_TYPE_SPACE_LOCATION};
    XR_CHECK(xrLocateSpace(pinchActionSpace, context.localSpace, sampleTime, &spaceLocation));
    CHECK((spaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0);
    CHECK_THAT(spaceLocation.pose.position.x, WithinAbs(0.15f, 0.001f));
    CHECK_THAT(spaceLocation.pose.position.y, WithinAbs(1.25f, 0.001f));
    CHECK_THAT(spaceLocation.pose.position.z, WithinAbs(-0.35f, 0.001f));

    std::array<XrSpace, 1> spaces = {pinchActionSpace};
    std::array<XrSpaceLocationData, 1> locationData = {};
    std::array<XrSpaceVelocityData, 1> velocityData = {};
    XrSpaceVelocities velocities = {XR_TYPE_SPACE_VELOCITIES};
    velocities.velocityCount = static_cast<uint32_t>(velocityData.size());
    velocities.velocities = velocityData.data();

    XrSpaceLocations spaceLocations = {XR_TYPE_SPACE_LOCATIONS};
    spaceLocations.next = &velocities;
    spaceLocations.locationCount = static_cast<uint32_t>(locationData.size());
    spaceLocations.locations = locationData.data();

    XrSpacesLocateInfo spacesLocateInfo = {XR_TYPE_SPACES_LOCATE_INFO};
    spacesLocateInfo.baseSpace = context.localSpace;
    spacesLocateInfo.time = sampleTime;
    spacesLocateInfo.spaceCount = static_cast<uint32_t>(spaces.size());
    spacesLocateInfo.spaces = spaces.data();

    XR_CHECK(xrLocateSpaces(context.session, &spacesLocateInfo, &spaceLocations));
    REQUIRE(spaceLocations.locationCount == 1);
    REQUIRE(velocities.velocityCount == 1);
    CHECK((locationData[0].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0);
    CHECK_THAT(locationData[0].pose.position.x, WithinAbs(0.15f, 0.001f));
    CHECK_THAT(locationData[0].pose.position.y, WithinAbs(1.25f, 0.001f));
    CHECK_THAT(locationData[0].pose.position.z, WithinAbs(-0.35f, 0.001f));
    CHECK((velocityData[0].velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0);
    CHECK((velocityData[0].velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0);
    CHECK_THAT(velocityData[0].linearVelocity.x, WithinAbs(0.0f, 0.001f));
    CHECK_THAT(velocityData[0].linearVelocity.y, WithinAbs(0.0f, 0.001f));
    CHECK_THAT(velocityData[0].linearVelocity.z, WithinAbs(0.0f, 0.001f));
    CHECK_THAT(velocityData[0].angularVelocity.x, WithinAbs(0.0f, 0.001f));
    CHECK_THAT(velocityData[0].angularVelocity.y, WithinAbs(0.0f, 0.001f));
    CHECK_THAT(velocityData[0].angularVelocity.z, WithinAbs(0.0f, 0.001f));

    XrActionStateGetInfo valueGetInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    valueGetInfo.action = pinchValueAction;
    valueGetInfo.subactionPath = leftHandPath;
    XrActionStateFloat valueState = {XR_TYPE_ACTION_STATE_FLOAT};
    XR_CHECK(xrGetActionStateFloat(context.session, &valueGetInfo, &valueState));
    CHECK(valueState.isActive == XR_TRUE);
    CHECK_THAT(valueState.currentState, WithinAbs(0.8f, 0.001f));

    XR_CHECK(xrDestroySpace(pinchActionSpace));
    XR_CHECK(xrDestroyAction(pinchPoseAction));
    XR_CHECK(xrDestroyAction(pinchValueAction));
    XR_CHECK(xrDestroyActionSet(actionSet));
}

TEST_CASE("Hand tracking rejects zero locate time", "[runtime][hand]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_HAND_TRACKING_EXTENSION_NAME,
    });

    auto createHandTracker = reinterpret_cast<PFN_xrCreateHandTrackerEXT>(
        GetProc(context.instance, "xrCreateHandTrackerEXT"));
    auto destroyHandTracker = reinterpret_cast<PFN_xrDestroyHandTrackerEXT>(
        GetProc(context.instance, "xrDestroyHandTrackerEXT"));
    auto locateHandJoints = reinterpret_cast<PFN_xrLocateHandJointsEXT>(
        GetProc(context.instance, "xrLocateHandJointsEXT"));

    XrHandTrackerCreateInfoEXT createInfo = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
    createInfo.hand = XR_HAND_LEFT_EXT;
    createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;

    XrHandTrackerEXT handTracker = XR_NULL_HANDLE;
    XR_CHECK(createHandTracker(context.session, &createInfo, &handTracker));

    std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> jointLocations = {};
    XrHandJointLocationsEXT locations = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
    locations.jointCount = static_cast<uint32_t>(jointLocations.size());
    locations.jointLocations = jointLocations.data();

    XrHandJointsLocateInfoEXT locateInfo = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
    locateInfo.baseSpace = context.localSpace;
    locateInfo.time = 0;

    CHECK(locateHandJoints(handTracker, &locateInfo, &locations) == XR_ERROR_TIME_INVALID);

    XR_CHECK(destroyHandTracker(handTracker));
}

TEST_CASE("Hand tracking validates joint buffer sizes and clears inactive flags", "[runtime][hand]")
{
    RuntimeSessionContext context({
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_HAND_TRACKING_EXTENSION_NAME,
    });

    auto createHandTracker = reinterpret_cast<PFN_xrCreateHandTrackerEXT>(
        GetProc(context.instance, "xrCreateHandTrackerEXT"));
    auto destroyHandTracker = reinterpret_cast<PFN_xrDestroyHandTrackerEXT>(
        GetProc(context.instance, "xrDestroyHandTrackerEXT"));
    auto locateHandJoints = reinterpret_cast<PFN_xrLocateHandJointsEXT>(
        GetProc(context.instance, "xrLocateHandJointsEXT"));

    XrHandTrackerCreateInfoEXT createInfo = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
    createInfo.hand = XR_HAND_LEFT_EXT;
    createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;

    XrHandTrackerEXT handTracker = XR_NULL_HANDLE;
    XR_CHECK(createHandTracker(context.session, &createInfo, &handTracker));

    std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> jointLocations = {};
    for (auto& jointLocation : jointLocations)
    {
        jointLocation.locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT;
    }

    XrHandJointsLocateInfoEXT locateInfo = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
    locateInfo.baseSpace = context.localSpace;
    locateInfo.time = 1;

    XrHandJointLocationsEXT locations = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
    locations.jointCount = static_cast<uint32_t>(jointLocations.size());
    locations.jointLocations = jointLocations.data();

    XR_CHECK(locateHandJoints(handTracker, &locateInfo, &locations));
    CHECK(locations.isActive == XR_FALSE);
    for (const auto& jointLocation : jointLocations)
    {
        CHECK(jointLocation.locationFlags == 0);
    }

    locations.jointCount = XR_HAND_JOINT_COUNT_EXT - 1;
    CHECK(locateHandJoints(handTracker, &locateInfo, &locations) == XR_ERROR_VALIDATION_FAILURE);

    std::array<XrHandJointVelocityEXT, XR_HAND_JOINT_COUNT_EXT> jointVelocities = {};
    XrHandJointVelocitiesEXT velocities = {XR_TYPE_HAND_JOINT_VELOCITIES_EXT};
    velocities.jointCount = XR_HAND_JOINT_COUNT_EXT - 1;
    velocities.jointVelocities = jointVelocities.data();

    locations.next = &velocities;
    locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
    CHECK(locateHandJoints(handTracker, &locateInfo, &locations) == XR_ERROR_VALIDATION_FAILURE);

    XR_CHECK(destroyHandTracker(handTracker));
}

TEST_CASE("Frame API supports pipelined waits and discarded begin semantics", "[runtime][frame]")
{
    RuntimeSessionContext context({XR_KHR_METAL_ENABLE_EXTENSION_NAME});

    XrFrameState firstFrameState = {XR_TYPE_FRAME_STATE};
    XrFrameState secondFrameState = {XR_TYPE_FRAME_STATE};
    XrFrameEndInfo frameEndInfo = {XR_TYPE_FRAME_END_INFO};
    frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    XR_CHECK(xrWaitFrame(context.session, nullptr, &firstFrameState));
    CHECK(xrBeginFrame(context.session, nullptr) == XR_SUCCESS);

    XR_CHECK(xrWaitFrame(context.session, nullptr, &secondFrameState));
    CHECK(xrBeginFrame(context.session, nullptr) == XR_FRAME_DISCARDED);

    frameEndInfo.displayTime = secondFrameState.predictedDisplayTime;
    CHECK(xrEndFrame(context.session, &frameEndInfo) == XR_SUCCESS);
    CHECK(xrBeginFrame(context.session, nullptr) == XR_ERROR_CALL_ORDER_INVALID);

    XrFrameState invalidTimeFrameState = {XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(context.session, nullptr, &invalidTimeFrameState));
    CHECK(xrBeginFrame(context.session, nullptr) == XR_SUCCESS);

    XrFrameEndInfo invalidTimeEndInfo = frameEndInfo;
    invalidTimeEndInfo.displayTime = 0;
    CHECK(xrEndFrame(context.session, &invalidTimeEndInfo) == XR_ERROR_TIME_INVALID);

    XrFrameState recoveredFrameState = {XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(context.session, nullptr, &recoveredFrameState));
    CHECK(xrBeginFrame(context.session, nullptr) == XR_FRAME_DISCARDED);

    frameEndInfo.displayTime = recoveredFrameState.predictedDisplayTime;
    CHECK(xrEndFrame(context.session, &frameEndInfo) == XR_SUCCESS);
}

TEST_CASE("EndFrame rejects invalid projection and quad layers", "[runtime][frame][layers]")
{
    auto runProjectionCase =
        [](auto configureProjection, XrResult expectedResult)
        {
            RuntimeSessionContext context({XR_KHR_METAL_ENABLE_EXTENSION_NAME});
            XrFrameState frameState = {XR_TYPE_FRAME_STATE};
            XR_CHECK(xrWaitFrame(context.session, nullptr, &frameState));
            XR_BEGIN_FRAME_CHECK(xrBeginFrame(context.session, nullptr));

            const int64_t format = SelectColorSwapchainFormat(context.session);
            XrSwapchain swapchain = CreateColorSwapchain(context.session, format);

            XrCompositionLayerProjectionView projectionViews[2] = {};
            for (auto& view : projectionViews)
            {
                view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                view.pose.orientation.w = 1.0f;
                view.fov.angleLeft = -0.5f;
                view.fov.angleRight = 0.5f;
                view.fov.angleUp = 0.5f;
                view.fov.angleDown = -0.5f;
                view.subImage.swapchain = swapchain;
                view.subImage.imageRect.extent = {16, 16};
            }

            configureProjection(context, swapchain, projectionViews);

            XrCompositionLayerProjection projectionLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projectionLayer.space = context.localSpace;
            projectionLayer.viewCount = 2;
            projectionLayer.views = projectionViews;

            const XrCompositionLayerBaseHeader* layers[] = {
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer),
            };

            XrFrameEndInfo frameEndInfo = {XR_TYPE_FRAME_END_INFO};
            frameEndInfo.displayTime = frameState.predictedDisplayTime;
            frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            frameEndInfo.layerCount = 1;
            frameEndInfo.layers = layers;

            CHECK(xrEndFrame(context.session, &frameEndInfo) == expectedResult);
            XR_CHECK(xrDestroySwapchain(swapchain));
        };

    auto runQuadCase =
        [](auto configureQuad, XrResult expectedResult)
        {
            RuntimeSessionContext context({XR_KHR_METAL_ENABLE_EXTENSION_NAME});
            XrFrameState frameState = {XR_TYPE_FRAME_STATE};
            XR_CHECK(xrWaitFrame(context.session, nullptr, &frameState));
            XR_BEGIN_FRAME_CHECK(xrBeginFrame(context.session, nullptr));

            const int64_t format = SelectColorSwapchainFormat(context.session);
            XrSwapchain swapchain = CreateColorSwapchain(context.session, format);

            uint32_t imageIndex = 0;
            XR_CHECK(xrAcquireSwapchainImage(swapchain, nullptr, &imageIndex));
            XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = 0;
            XR_CHECK(xrWaitSwapchainImage(swapchain, &waitInfo));
            XR_CHECK(xrReleaseSwapchainImage(swapchain, nullptr));

            XrCompositionLayerQuad quadLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            quadLayer.space = context.localSpace;
            quadLayer.pose.orientation.w = 1.0f;
            quadLayer.size = {1.0f, 1.0f};
            quadLayer.subImage.swapchain = swapchain;
            quadLayer.subImage.imageRect.extent = {16, 16};

            configureQuad(context, swapchain, quadLayer);

            const XrCompositionLayerBaseHeader* layers[] = {
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quadLayer),
            };

            XrFrameEndInfo frameEndInfo = {XR_TYPE_FRAME_END_INFO};
            frameEndInfo.displayTime = frameState.predictedDisplayTime;
            frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            frameEndInfo.layerCount = 1;
            frameEndInfo.layers = layers;

            CHECK(xrEndFrame(context.session, &frameEndInfo) == expectedResult);
            XR_CHECK(xrDestroySwapchain(swapchain));
        };

    runProjectionCase(
        [](const RuntimeSessionContext&, XrSwapchain, XrCompositionLayerProjectionView (&)[2]) {},
        XR_ERROR_LAYER_INVALID);

    runProjectionCase(
        [](const RuntimeSessionContext&, XrSwapchain swapchain, XrCompositionLayerProjectionView (&views)[2])
        {
            uint32_t imageIndex = 0;
            XR_CHECK(xrAcquireSwapchainImage(swapchain, nullptr, &imageIndex));
            XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = 0;
            XR_CHECK(xrWaitSwapchainImage(swapchain, &waitInfo));
            XR_CHECK(xrReleaseSwapchainImage(swapchain, nullptr));
            views[0].subImage.imageRect.offset.x = -1;
        },
        XR_ERROR_SWAPCHAIN_RECT_INVALID);

    runProjectionCase(
        [](const RuntimeSessionContext&, XrSwapchain swapchain, XrCompositionLayerProjectionView (&views)[2])
        {
            uint32_t imageIndex = 0;
            XR_CHECK(xrAcquireSwapchainImage(swapchain, nullptr, &imageIndex));
            XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = 0;
            XR_CHECK(xrWaitSwapchainImage(swapchain, &waitInfo));
            XR_CHECK(xrReleaseSwapchainImage(swapchain, nullptr));
            views[0].pose.orientation.w = 0.0f;
        },
        XR_ERROR_POSE_INVALID);

    runQuadCase(
        [](const RuntimeSessionContext&, XrSwapchain, XrCompositionLayerQuad& quad)
        {
            quad.subImage.imageArrayIndex = 1;
        },
        XR_ERROR_VALIDATION_FAILURE);
}

TEST_CASE("EndFrame accepts a released projection image while another swapchain image is acquired", "[runtime][frame][swapchain]")
{
    RuntimeSessionContext context({XR_KHR_METAL_ENABLE_EXTENSION_NAME});
    XrFrameState frameState = {XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(context.session, nullptr, &frameState));
    XR_BEGIN_FRAME_CHECK(xrBeginFrame(context.session, nullptr));

    const int64_t format = SelectColorSwapchainFormat(context.session);
    XrSwapchain swapchain = CreateColorSwapchain(context.session, format);

    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = 0;

    uint32_t releasedImageIndex = 0;
    XR_CHECK(xrAcquireSwapchainImage(swapchain, nullptr, &releasedImageIndex));
    XR_CHECK(xrWaitSwapchainImage(swapchain, &waitInfo));
    XR_CHECK(xrReleaseSwapchainImage(swapchain, nullptr));

    uint32_t pipelinedImageIndex = 0;
    XR_CHECK(xrAcquireSwapchainImage(swapchain, nullptr, &pipelinedImageIndex));

    XrCompositionLayerProjectionView projectionViews[2] = {};
    for (auto& view : projectionViews)
    {
        view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        view.pose.orientation.w = 1.0f;
        view.fov.angleLeft = -0.5f;
        view.fov.angleRight = 0.5f;
        view.fov.angleUp = 0.5f;
        view.fov.angleDown = -0.5f;
        view.subImage.swapchain = swapchain;
        view.subImage.imageRect.extent = {16, 16};
    }

    XrCompositionLayerProjection projectionLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.space = context.localSpace;
    projectionLayer.viewCount = 2;
    projectionLayer.views = projectionViews;

    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer),
    };

    XrFrameEndInfo frameEndInfo = {XR_TYPE_FRAME_END_INFO};
    frameEndInfo.displayTime = frameState.predictedDisplayTime;
    frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    frameEndInfo.layerCount = 1;
    frameEndInfo.layers = layers;

    CHECK(xrEndFrame(context.session, &frameEndInfo) == XR_SUCCESS);

    XR_CHECK(xrWaitSwapchainImage(swapchain, &waitInfo));
    XR_CHECK(xrReleaseSwapchainImage(swapchain, nullptr));
    XR_CHECK(xrDestroySwapchain(swapchain));
}

TEST_CASE("Space and view validation matches CTS expectations", "[runtime][space]")
{
    RuntimeSessionContext context({XR_KHR_METAL_ENABLE_EXTENSION_NAME});

    XrExtent2Df bounds = {};
    CHECK(xrGetReferenceSpaceBoundsRect(context.session, XR_REFERENCE_SPACE_TYPE_MAX_ENUM, &bounds) ==
          XR_ERROR_REFERENCE_SPACE_UNSUPPORTED);

    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = 0;
    locateInfo.space = context.localSpace;

    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    uint32_t viewCountOutput = 0;
    std::array<XrView, 2> views = {{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    CHECK(xrLocateViews(context.session, &locateInfo, &viewState,
                        static_cast<uint32_t>(views.size()), &viewCountOutput, views.data()) ==
          XR_ERROR_TIME_INVALID);

    locateInfo.displayTime = -42;
    CHECK(xrLocateViews(context.session, &locateInfo, &viewState,
                        static_cast<uint32_t>(views.size()), &viewCountOutput, views.data()) ==
          XR_ERROR_TIME_INVALID);

    locateInfo.displayTime = 1;
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
    CHECK(xrLocateViews(context.session, &locateInfo, &viewState,
                        static_cast<uint32_t>(views.size()), &viewCountOutput, views.data()) ==
          XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);

    XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
    CHECK(xrLocateSpace(context.localSpace, context.localSpace, 0, &location) ==
          XR_ERROR_TIME_INVALID);
    CHECK(xrLocateSpace(context.localSpace, context.localSpace, -42, &location) ==
          XR_ERROR_TIME_INVALID);
}

TEST_CASE("Swapchain image order follows acquire wait release rules", "[runtime][swapchain]")
{
    RuntimeSessionContext context({XR_KHR_METAL_ENABLE_EXTENSION_NAME});

    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = 0;

    int64_t format = SelectColorSwapchainFormat(context.session);
    XrSwapchain swapchain = CreateColorSwapchain(context.session, format);

    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr));
    REQUIRE(imageCount > 0);

    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    CHECK(xrWaitSwapchainImage(swapchain, &waitInfo) == XR_ERROR_CALL_ORDER_INVALID);
    CHECK(xrReleaseSwapchainImage(swapchain, &releaseInfo) == XR_ERROR_CALL_ORDER_INVALID);

    std::vector<uint32_t> acquiredIndices(imageCount, UINT32_MAX);
    for (uint32_t i = 0; i < imageCount; ++i)
    {
        XR_CHECK(xrAcquireSwapchainImage(swapchain, nullptr, &acquiredIndices[i]));
        CHECK(xrReleaseSwapchainImage(swapchain, &releaseInfo) == XR_ERROR_CALL_ORDER_INVALID);
    }

    uint32_t extraIndex = UINT32_MAX;
    CHECK(xrAcquireSwapchainImage(swapchain, nullptr, &extraIndex) == XR_ERROR_CALL_ORDER_INVALID);

    for (uint32_t i = 0; i < imageCount; ++i)
    {
        XR_CHECK(xrWaitSwapchainImage(swapchain, &waitInfo));
        CHECK(xrWaitSwapchainImage(swapchain, &waitInfo) == XR_ERROR_CALL_ORDER_INVALID);
        XR_CHECK(xrReleaseSwapchainImage(swapchain, nullptr));
    }

    XR_CHECK(xrDestroySwapchain(swapchain));

    XrSwapchain staticSwapchain =
        CreateColorSwapchain(context.session, format, XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT);
    XR_CHECK(xrAcquireSwapchainImage(staticSwapchain, nullptr, &extraIndex));
    XR_CHECK(xrWaitSwapchainImage(staticSwapchain, &waitInfo));
    XR_CHECK(xrReleaseSwapchainImage(staticSwapchain, nullptr));
    CHECK(xrAcquireSwapchainImage(staticSwapchain, nullptr, &extraIndex) == XR_ERROR_CALL_ORDER_INVALID);
    XR_CHECK(xrDestroySwapchain(staticSwapchain));
}
