// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include <openxr/openxr.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <vector>

namespace
{

void CheckXr(XrResult result, const char* expression)
{
    INFO(expression);
    REQUIRE(result == XR_SUCCESS);
}

#define XR_CHECK(expr) CheckXr((expr), #expr)

bool HasInstanceExtension(const char* extensionName)
{
    uint32_t extensionCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
    std::vector<XrExtensionProperties> extensions(
        extensionCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(
        nullptr, extensionCount, &extensionCount, extensions.data()));

    return std::any_of(extensions.begin(), extensions.end(),
                       [extensionName](const XrExtensionProperties& extension)
                       {
                           return std::strcmp(extension.extensionName, extensionName) == 0;
                       });
}

XrInstance CreateInstance(const std::vector<const char*>& extensions)
{
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, "OXRSys Windows D3D test",
                 XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.applicationVersion = 1;
    std::strncpy(createInfo.applicationInfo.engineName, "OXRSys tests",
                 XR_MAX_ENGINE_NAME_SIZE);
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.enabledExtensionNames = extensions.data();

    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&createInfo, &instance));
    return instance;
}

XrSystemId GetSystem(XrInstance instance)
{
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance, &systemInfo, &systemId));
    return systemId;
}

PFN_xrVoidFunction GetProc(XrInstance instance, const char* name)
{
    PFN_xrVoidFunction function = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(instance, name, &function));
    REQUIRE(function != nullptr);
    return function;
}

Microsoft::WRL::ComPtr<ID3D11Device> CreateD3D11WarpDevice()
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selectedLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        0,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &device,
        &selectedLevel,
        &context);
    if (FAILED(result))
    {
        SKIP("D3D11 WARP device is unavailable");
    }
    return device;
}

struct D3D12WarpDevice
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
};

D3D12WarpDevice CreateD3D12WarpDevice()
{
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        SKIP("DXGI factory is unavailable");
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> warpAdapter;
    if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter))))
    {
        SKIP("DXGI WARP adapter is unavailable");
    }

    D3D12WarpDevice result;
    if (FAILED(D3D12CreateDevice(
            warpAdapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&result.device))))
    {
        SKIP("D3D12 WARP device is unavailable");
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(result.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&result.queue))))
    {
        SKIP("D3D12 WARP command queue is unavailable");
    }
    return result;
}

std::vector<int64_t> EnumerateFormats(XrSession session)
{
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr));
    REQUIRE(formatCount > 0);
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()));
    return formats;
}

XrSwapchain CreateColorSwapchain(XrSession session, int64_t format)
{
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = format;
    createInfo.sampleCount = 1;
    createInfo.width = 16;
    createInfo.height = 16;
    createInfo.faceCount = 1;
    createInfo.arraySize = 2;
    createInfo.mipCount = 1;

    XrSwapchain swapchain = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSwapchain(session, &createInfo, &swapchain));
    return swapchain;
}

void CheckInvalidSwapchainCreation(XrSession session, int64_t validFormat)
{
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = validFormat;
    createInfo.sampleCount = 1;
    createInfo.width = 16;
    createInfo.height = 16;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;

    XrSwapchain swapchain = XR_NULL_HANDLE;
    XrSwapchainCreateInfo invalidFormatInfo = createInfo;
    invalidFormatInfo.format = DXGI_FORMAT_UNKNOWN;
    CHECK(xrCreateSwapchain(session, &invalidFormatInfo, &swapchain) ==
          XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
    CHECK(swapchain == XR_NULL_HANDLE);

    XrSwapchainCreateInfo invalidShapeInfo = createInfo;
    invalidShapeInfo.height = 0;
    CHECK(xrCreateSwapchain(session, &invalidShapeInfo, &swapchain) ==
          XR_ERROR_VALIDATION_FAILURE);
    CHECK(swapchain == XR_NULL_HANDLE);

    invalidShapeInfo = createInfo;
    invalidShapeInfo.mipCount = 2;
    CHECK(xrCreateSwapchain(session, &invalidShapeInfo, &swapchain) ==
          XR_ERROR_FEATURE_UNSUPPORTED);
    CHECK(swapchain == XR_NULL_HANDLE);
}

void SubmitProjectionFrame(XrSession session, XrSwapchain swapchain)
{
    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XR_CHECK(xrBeginSession(session, &beginInfo));

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

    XrSpace space = XR_NULL_HANDLE;
    XR_CHECK(xrCreateReferenceSpace(session, &spaceInfo, &space));

    XrFrameWaitInfo waitFrameInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(session, &waitFrameInfo, &frameState));

    XrFrameBeginInfo beginFrameInfo{XR_TYPE_FRAME_BEGIN_INFO};
    XR_CHECK(xrBeginFrame(session, &beginFrameInfo));

    std::array<XrCompositionLayerProjectionView, 2> views = {
        XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
    };
    for (uint32_t eye = 0; eye < views.size(); ++eye)
    {
        views[eye].pose.orientation.w = 1.0f;
        views[eye].fov.angleLeft = -0.5f;
        views[eye].fov.angleRight = 0.5f;
        views[eye].fov.angleUp = 0.5f;
        views[eye].fov.angleDown = -0.5f;
        views[eye].subImage.swapchain = swapchain;
        views[eye].subImage.imageRect.extent = {16, 16};
        views[eye].subImage.imageArrayIndex = eye;
    }

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = space;
    layer.viewCount = static_cast<uint32_t>(views.size());
    layer.views = views.data();

    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer),
    };
    XrFrameEndInfo endFrameInfo{XR_TYPE_FRAME_END_INFO};
    endFrameInfo.displayTime = frameState.predictedDisplayTime;
    endFrameInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endFrameInfo.layerCount = 1;
    endFrameInfo.layers = layers;
    XR_CHECK(xrEndFrame(session, &endFrameInfo));

    XR_CHECK(xrDestroySpace(space));
}

} // namespace

TEST_CASE("Windows runtime advertises D3D extensions")
{
    REQUIRE(HasInstanceExtension(XR_KHR_D3D11_ENABLE_EXTENSION_NAME));
    REQUIRE(HasInstanceExtension(XR_KHR_D3D12_ENABLE_EXTENSION_NAME));
}

TEST_CASE("D3D11 requirements are required before session creation")
{
    XrInstance instance = CreateInstance({XR_KHR_D3D11_ENABLE_EXTENSION_NAME});
    XrSystemId systemId = GetSystem(instance);
    auto device = CreateD3D11WarpDevice();

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device.Get();
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = systemId;

    XrSession session = XR_NULL_HANDLE;
    CHECK(xrCreateSession(instance, &sessionInfo, &session) ==
          XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);
    XR_CHECK(xrDestroyInstance(instance));
}

TEST_CASE("D3D11 session and swapchain images can be created")
{
    XrInstance instance = CreateInstance({XR_KHR_D3D11_ENABLE_EXTENSION_NAME});
    XrSystemId systemId = GetSystem(instance);

    auto getRequirements = reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(
        GetProc(instance, "xrGetD3D11GraphicsRequirementsKHR"));
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_CHECK(getRequirements(instance, systemId, &requirements));
    CHECK(requirements.minFeatureLevel >= D3D_FEATURE_LEVEL_11_0);

    auto device = CreateD3D11WarpDevice();
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device.Get();
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = systemId;

    XrSession session = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSession(instance, &sessionInfo, &session));

    auto formats = EnumerateFormats(session);
    REQUIRE(std::find(formats.begin(), formats.end(), DXGI_FORMAT_R8G8B8A8_UNORM) != formats.end());
    CheckInvalidSwapchainCreation(session, DXGI_FORMAT_R8G8B8A8_UNORM);

    XrSwapchain swapchain = CreateColorSwapchain(session, DXGI_FORMAT_R8G8B8A8_UNORM);
    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr));
    REQUIRE(imageCount > 0);
    std::vector<XrSwapchainImageD3D11KHR> images(
        imageCount, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(
        swapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
    CHECK(images.front().texture != nullptr);

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XR_CHECK(xrAcquireSwapchainImage(swapchain, &acquireInfo, &imageIndex));
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrWaitSwapchainImage(swapchain, &waitInfo));
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_CHECK(xrReleaseSwapchainImage(swapchain, &releaseInfo));
    SubmitProjectionFrame(session, swapchain);

    XR_CHECK(xrDestroySwapchain(swapchain));
    XR_CHECK(xrDestroySession(session));
    XR_CHECK(xrDestroyInstance(instance));
}

TEST_CASE("D3D12 session and swapchain images can be created")
{
    XrInstance instance = CreateInstance({XR_KHR_D3D12_ENABLE_EXTENSION_NAME});
    XrSystemId systemId = GetSystem(instance);

    auto getRequirements = reinterpret_cast<PFN_xrGetD3D12GraphicsRequirementsKHR>(
        GetProc(instance, "xrGetD3D12GraphicsRequirementsKHR"));
    XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    XR_CHECK(getRequirements(instance, systemId, &requirements));
    CHECK(requirements.minFeatureLevel >= D3D_FEATURE_LEVEL_11_0);

    D3D12WarpDevice d3d12 = CreateD3D12WarpDevice();
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = d3d12.device.Get();
    binding.queue = d3d12.queue.Get();
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = systemId;

    XrSession session = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSession(instance, &sessionInfo, &session));

    auto formats = EnumerateFormats(session);
    REQUIRE(std::find(formats.begin(), formats.end(), DXGI_FORMAT_B8G8R8A8_UNORM) != formats.end());
    CheckInvalidSwapchainCreation(session, DXGI_FORMAT_B8G8R8A8_UNORM);

    XrSwapchain swapchain = CreateColorSwapchain(session, DXGI_FORMAT_B8G8R8A8_UNORM);
    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr));
    REQUIRE(imageCount > 0);
    std::vector<XrSwapchainImageD3D12KHR> images(
        imageCount, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(
        swapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
    CHECK(images.front().texture != nullptr);

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XR_CHECK(xrAcquireSwapchainImage(swapchain, &acquireInfo, &imageIndex));
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrWaitSwapchainImage(swapchain, &waitInfo));
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_CHECK(xrReleaseSwapchainImage(swapchain, &releaseInfo));
    SubmitProjectionFrame(session, swapchain);

    XR_CHECK(xrDestroySwapchain(swapchain));
    XR_CHECK(xrDestroySession(session));
    XR_CHECK(xrDestroyInstance(instance));
}
