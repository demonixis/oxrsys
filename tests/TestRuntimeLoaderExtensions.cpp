// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include <openxr/openxr.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{

constexpr const char* kVulkanEnableExtensionName = "XR_KHR_vulkan_enable";
constexpr const char* kVulkanEnable2ExtensionName = "XR_KHR_vulkan_enable2";
constexpr const char* kMetalEnableExtensionName = "XR_KHR_metal_enable";
constexpr const char* kOpenGLEnableExtensionName = "XR_KHR_opengl_enable";
constexpr const char* kD3D11EnableExtensionName = "XR_KHR_D3D11_enable";
constexpr const char* kD3D12EnableExtensionName = "XR_KHR_D3D12_enable";
constexpr const char* kMetalObjectsDeviceExtensionName = "VK_EXT_metal_objects";
using GetVulkanDeviceExtensions = XrResult (*)(
    XrInstance, XrSystemId, uint32_t, uint32_t*, char*);

void CheckXr(XrResult result, const char* expression)
{
    INFO(expression);
    REQUIRE(result == XR_SUCCESS);
}

#define XR_CHECK(expr) CheckXr((expr), #expr)

std::vector<std::string> EnumerateRuntimeExtensions()
{
    uint32_t extensionCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));

    std::vector<XrExtensionProperties> properties(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount,
                                                    properties.data()));

    std::vector<std::string> names;
    names.reserve(properties.size());
    for (const auto& property : properties)
    {
        names.emplace_back(property.extensionName);
    }
    return names;
}

bool HasExtension(const std::vector<std::string>& extensions, const char* extensionName)
{
    return std::find(extensions.begin(), extensions.end(), extensionName) != extensions.end();
}

XrInstance CreateInstanceWithExtensions(const std::vector<const char*>& extensions)
{
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName,
                 "oxrsys_loader_extension_tests",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.enabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));
    return instance;
}

} // namespace

TEST_CASE("Runtime advertises only macOS graphics extensions",
          "[runtime][loader][graphics]")
{
    const std::vector<std::string> extensions = EnumerateRuntimeExtensions();

    CHECK(HasExtension(extensions, kMetalEnableExtensionName));
    CHECK(HasExtension(extensions, kVulkanEnableExtensionName));
    CHECK(HasExtension(extensions, kVulkanEnable2ExtensionName));
    CHECK_FALSE(HasExtension(extensions, kOpenGLEnableExtensionName));
    CHECK_FALSE(HasExtension(extensions, kD3D11EnableExtensionName));
    CHECK_FALSE(HasExtension(extensions, kD3D12EnableExtensionName));
}

TEST_CASE("Removed graphics entry points are unsupported",
          "[runtime][loader][graphics]")
{
    XrInstance instance = CreateInstanceWithExtensions({});
    PFN_xrVoidFunction function = nullptr;
    CHECK(xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR", &function) ==
          XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(function == nullptr);
    function = nullptr;
    CHECK(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR", &function) ==
          XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(function == nullptr);
    function = nullptr;
    CHECK(xrGetInstanceProcAddr(instance, "xrGetD3D12GraphicsRequirementsKHR", &function) ==
          XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(function == nullptr);
    XR_CHECK(xrDestroyInstance(instance));
}

TEST_CASE("Vulkan v1 requires MoltenVK Metal interop",
          "[runtime][loader][graphics]")
{
    XrInstance instance = CreateInstanceWithExtensions({kVulkanEnableExtensionName});
    PFN_xrVoidFunction function = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(instance, "xrGetVulkanDeviceExtensionsKHR", &function));
    auto getDeviceExtensions = reinterpret_cast<GetVulkanDeviceExtensions>(function);
    REQUIRE(getDeviceExtensions != nullptr);

    uint32_t size = 0;
    XR_CHECK(getDeviceExtensions(instance, 1, 0, &size, nullptr));
    REQUIRE(size > 1);
    std::vector<char> extensions(size);
    XR_CHECK(getDeviceExtensions(instance, 1, size, &size, extensions.data()));
    const std::string extensionList(extensions.data());
    CHECK(extensionList.find(kMetalObjectsDeviceExtensionName) != std::string::npos);
    CHECK(extensionList.find("VK_KHR_portability_subset") != std::string::npos);

    XR_CHECK(xrDestroyInstance(instance));
}
