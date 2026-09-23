// Frame VR client — V0: hello stereo
// SPDX-License-Identifier: BSL-1.0
//
// Minimal Vulkan + OpenXR stereo client. Proves the spine every later
// milestone builds on: instance/session, Vulkan binding, two-view stereo
// projection layer, 6DoF head tracking, and an extension-enumeration dump
// (the on-hardware deliverable for the Frame). No windowing — the runtime
// owns presentation (hosted class), so this is pure portable C++ that runs
// against DisplayXR (MoltenVK) on the Mac today and retargets to the Frame's
// runtime by only swapping loader discovery.
//
// V0 renders a flat clear color per eye (left reddish, right bluish) — enough
// to confirm the stereo path without any pipeline/shader/geometry. Real
// per-eye content (decoded video) arrives in V1/V2.

#define XR_USE_GRAPHICS_API_VULKAN

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "video_decoder.h"
#include "stream_connection.h"
#include "xr_input.h"

#include <oxrsys/protocol/Protocol.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <unistd.h>

#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call)                                                         \
    do {                                                                       \
        XrResult _r = (call);                                                  \
        if (XR_FAILED(_r)) {                                                   \
            LOG_ERROR("OpenXR error %d at %s:%d", (int)_r, __FILE__, __LINE__);\
            return false;                                                      \
        }                                                                      \
    } while (0)

#define VK_CHECK(call)                                                         \
    do {                                                                       \
        VkResult _r = (call);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            LOG_ERROR("Vulkan error %d at %s:%d", (int)_r, __FILE__, __LINE__);\
            return false;                                                      \
        }                                                                      \
    } while (0)

// ============================================================================
// State
// ============================================================================

static constexpr uint32_t VIEW_COUNT = 2; // PRIMARY_STEREO

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0, height = 0;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
};

struct App {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace stageSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;  // head center, for the pose sent upstream
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool exitRequested = false;

    std::vector<XrViewConfigurationView> configViews;
    bool hasHandTracking = false;
    bool hasEyeGaze = false;
    bool hasRefreshRate = false;

    // Vulkan
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    Swapchain swapchains[VIEW_COUNT];

    // V1: video texture + sampling pipeline
    VkImage videoImage = VK_NULL_HANDLE;
    VkDeviceMemory videoMem = VK_NULL_HANDLE;
    VkImageView videoView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t videoW = 0, videoH = 0;
    VkImageLayout videoLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    VkDeviceSize stagingSize = 0;

    VkDescriptorSetLayout dsLayout = VK_NULL_HANDLE;
    VkDescriptorPool dsPool = VK_NULL_HANDLE;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool descriptorWritten = false;
};

struct PushConstants {
    float sourceMin[2];
    float sourceMax[2];
    float centerSize[2];
    float centerShift[2];
    float edgeRatio[2];
    float eyeSizeRatio[2];
    int32_t foveated;
};

static VideoDecoder g_decoder;
static std::vector<uint8_t> g_frameBuf;
static std::string g_videoPath;
static StreamConnection g_stream;
static bool g_streamMode = false;
static XrInput g_xrInput;

static volatile bool g_running = true;
static void SignalHandler(int)
{
    if (!g_running) _exit(0); // second Ctrl-C: hard exit if graceful teardown hangs
    g_running = false;
}

// ============================================================================
// OpenXR: instance + extension dump (the V0 deliverable)
// ============================================================================

static bool CreateInstance(App& app)
{
    uint32_t count = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
    std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data()));

    // ---- Extension enumeration dump: the confirmation we need on the Frame ----
    LOG_INFO("==== OpenXR runtime exposes %u instance extensions ====", count);
    bool hasVk = false, hasVk2 = false, hasGLES = false;
    for (const auto& e : exts) {
        LOG_INFO("  %-48s v%u", e.extensionName, e.extensionVersion);
        if (!strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))  hasVk = true;
        if (!strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) hasVk2 = true;
        if (!strcmp(e.extensionName, "XR_KHR_opengl_es_enable"))            hasGLES = true;
    }
    LOG_INFO("==== graphics bindings: vulkan_enable=%d vulkan_enable2=%d opengl_es_enable=%d ====",
             hasVk, hasVk2, hasGLES);

    if (!hasVk) {
        LOG_ERROR("Runtime does not expose XR_KHR_vulkan_enable (V0 uses the v1 path)");
        return false;
    }

    auto has = [&](const char* n) {
        for (const auto& e : exts) if (!strcmp(e.extensionName, n)) return true;
        return false;
    };
    std::vector<const char*> enabled = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
    if (has(XR_EXT_HAND_TRACKING_EXTENSION_NAME)) {
        enabled.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        app.hasHandTracking = true;
    }
    if (has(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) {
        enabled.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        app.hasEyeGaze = true;
    }
    if (has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
        enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        app.hasRefreshRate = true;
    }

    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(ci.applicationInfo.applicationName, "FrameClient", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    // Request the baseline we use, not the header's newest — a runtime pinned to
    // an older 1.1.x patch rejects a too-new apiVersion (learned against oxrsys).
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 1, 0);
    ci.enabledExtensionCount = (uint32_t)enabled.size();
    ci.enabledExtensionNames = enabled.data();
    XR_CHECK(xrCreateInstance(&ci, &app.instance));
    LOG_INFO("hand tracking: %s, eye gaze: %s",
             app.hasHandTracking ? "enabled" : "unavailable",
             app.hasEyeGaze ? "enabled" : "unavailable");

    XrInstanceProperties props = {XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(app.instance, &props))) {
        LOG_INFO("Runtime: %s (%u.%u.%u)", props.runtimeName,
                 XR_VERSION_MAJOR(props.runtimeVersion),
                 XR_VERSION_MINOR(props.runtimeVersion),
                 XR_VERSION_PATCH(props.runtimeVersion));
    }
    return true;
}

static bool GetSystem(App& app)
{
    XrSystemGetInfo gi = {XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(app.instance, &gi, &app.systemId));

    uint32_t count = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(app.instance, app.systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr));
    app.configViews.assign(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(app.instance, app.systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, app.configViews.data()));
    LOG_INFO("PRIMARY_STEREO reports %u views", count);
    for (uint32_t i = 0; i < count; i++) {
        LOG_INFO("  view %u: recommended %ux%u", i,
                 app.configViews[i].recommendedImageRectWidth,
                 app.configViews[i].recommendedImageRectHeight);
    }
    if (count != VIEW_COUNT) {
        LOG_ERROR("Expected %u stereo views, got %u", VIEW_COUNT, count);
        return false;
    }
    return true;
}

// ============================================================================
// Vulkan setup (v1 path), following the OpenXR-required call order
// ============================================================================

template <typename T>
static bool xrProc(XrInstance inst, const char* name, T* out)
{
    return XR_SUCCEEDED(xrGetInstanceProcAddr(inst, name, (PFN_xrVoidFunction*)out));
}

static std::vector<std::string> splitSpaces(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == ' ' || s[i] == '\0') {
            if (i > start) out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

static bool CreateVulkan(App& app)
{
    // Spec requires xrGetVulkanGraphicsRequirementsKHR before creating VkInstance.
    PFN_xrGetVulkanGraphicsRequirementsKHR pfnReq = nullptr;
    if (!xrProc(app.instance, "xrGetVulkanGraphicsRequirementsKHR", &pfnReq)) return false;
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XR_CHECK(pfnReq(app.instance, app.systemId, &req));

    // Instance extensions the runtime requires.
    PFN_xrGetVulkanInstanceExtensionsKHR pfnInstExts = nullptr;
    if (!xrProc(app.instance, "xrGetVulkanInstanceExtensionsKHR", &pfnInstExts)) return false;
    uint32_t sz = 0;
    pfnInstExts(app.instance, app.systemId, 0, &sz, nullptr);
    std::string instExtStr(sz, '\0');
    pfnInstExts(app.instance, app.systemId, sz, &sz, instExtStr.data());
    std::vector<std::string> instStore = splitSpaces(instExtStr);
    std::vector<const char*> instPtrs;
    for (auto& s : instStore) instPtrs.push_back(s.c_str());

    // MoltenVK portability enumeration (Mac dev build; harmless elsewhere).
    uint32_t availCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availCount, nullptr);
    std::vector<VkExtensionProperties> avail(availCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availCount, avail.data());
    bool hasPortEnum = false;
    for (auto& e : avail)
        if (!strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) hasPortEnum = true;
    if (hasPortEnum) instPtrs.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "FrameClientV0";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = (uint32_t)instPtrs.size();
    ici.ppEnabledExtensionNames = instPtrs.data();
    if (hasPortEnum) ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &app.vkInstance));

    // Physical device chosen by the runtime.
    PFN_xrGetVulkanGraphicsDeviceKHR pfnDev = nullptr;
    if (!xrProc(app.instance, "xrGetVulkanGraphicsDeviceKHR", &pfnDev)) return false;
    XR_CHECK(pfnDev(app.instance, app.systemId, app.vkInstance, &app.physDevice));
    VkPhysicalDeviceProperties pdp;
    vkGetPhysicalDeviceProperties(app.physDevice, &pdp);
    LOG_INFO("Vulkan device: %s", pdp.deviceName);

    // Graphics queue family.
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(app.physDevice, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfam(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(app.physDevice, &qCount, qfam.data());
    bool foundQ = false;
    for (uint32_t i = 0; i < qCount; i++)
        if (qfam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { app.queueFamilyIndex = i; foundQ = true; break; }
    if (!foundQ) { LOG_ERROR("No graphics queue family"); return false; }

    // Device extensions the runtime requires + portability subset if present.
    PFN_xrGetVulkanDeviceExtensionsKHR pfnDevExts = nullptr;
    if (!xrProc(app.instance, "xrGetVulkanDeviceExtensionsKHR", &pfnDevExts)) return false;
    sz = 0;
    pfnDevExts(app.instance, app.systemId, 0, &sz, nullptr);
    std::string devExtStr(sz, '\0');
    pfnDevExts(app.instance, app.systemId, sz, &sz, devExtStr.data());

    uint32_t devAvailCount = 0;
    vkEnumerateDeviceExtensionProperties(app.physDevice, nullptr, &devAvailCount, nullptr);
    std::vector<VkExtensionProperties> devAvail(devAvailCount);
    vkEnumerateDeviceExtensionProperties(app.physDevice, nullptr, &devAvailCount, devAvail.data());
    auto devHas = [&](const char* n) {
        for (auto& e : devAvail) if (!strcmp(e.extensionName, n)) return true;
        return false;
    };

    std::vector<std::string> devStore;
    for (auto& s : splitSpaces(devExtStr)) if (devHas(s.c_str())) devStore.push_back(s);
    if (devHas("VK_KHR_portability_subset")) devStore.push_back("VK_KHR_portability_subset");
    std::vector<const char*> devPtrs;
    for (auto& s : devStore) devPtrs.push_back(s.c_str());

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = app.queueFamilyIndex;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devPtrs.size();
    dci.ppEnabledExtensionNames = devPtrs.data();
    VK_CHECK(vkCreateDevice(app.physDevice, &dci, nullptr, &app.device));
    vkGetDeviceQueue(app.device, app.queueFamilyIndex, 0, &app.queue);

    // Command pool + one reusable command buffer + a fence.
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = app.queueFamilyIndex;
    VK_CHECK(vkCreateCommandPool(app.device, &cpi, nullptr, &app.cmdPool));
    VkCommandBufferAllocateInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = app.cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(app.device, &cbi, &app.cmdBuffer));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(app.device, &fci, nullptr, &app.fence));

    LOG_INFO("Vulkan ready (queue family %u)", app.queueFamilyIndex);
    return true;
}

// ============================================================================
// Session, swapchains, render pass
// ============================================================================

static bool CreateSession(App& app)
{
    XrGraphicsBindingVulkanKHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = app.vkInstance;
    binding.physicalDevice = app.physDevice;
    binding.device = app.device;
    binding.queueFamilyIndex = app.queueFamilyIndex;
    binding.queueIndex = 0;

    XrSessionCreateInfo ci = {XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;              // hosted class: no window binding chained
    ci.systemId = app.systemId;
    XR_CHECK(xrCreateSession(app.instance, &ci, &app.session));

    XrReferenceSpaceCreateInfo si = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    si.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(xrCreateReferenceSpace(app.session, &si, &app.stageSpace));

    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(app.session, &si, &app.viewSpace));
    LOG_INFO("Session + stage/view spaces created");
    return true;
}

static int64_t ChooseSwapchainFormat(App& app)
{
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(app.session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(app.session, count, &count, formats.data());
    // Prefer a common SRGB color format; fall back to the runtime's first.
    const int64_t preferred[] = { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                                  VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };
    for (int64_t want : preferred)
        for (int64_t f : formats) if (f == want) return f;
    return formats.empty() ? (int64_t)VK_FORMAT_R8G8B8A8_UNORM : formats[0];
}

static bool CreateRenderPass(App& app, VkFormat format)
{
    VkAttachmentDescription color = {};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &color;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    VK_CHECK(vkCreateRenderPass(app.device, &rpi, nullptr, &app.renderPass));
    return true;
}

static bool CreateSwapchains(App& app)
{
    int64_t format = ChooseSwapchainFormat(app);
    LOG_INFO("Swapchain format: %lld", (long long)format);
    if (!CreateRenderPass(app, (VkFormat)format)) return false;

    for (uint32_t eye = 0; eye < VIEW_COUNT; eye++) {
        Swapchain& sc = app.swapchains[eye];
        sc.format = format;
        sc.width = app.configViews[eye].recommendedImageRectWidth;
        sc.height = app.configViews[eye].recommendedImageRectHeight;

        XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = sc.width;
        ci.height = sc.height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XR_CHECK(xrCreateSwapchain(app.session, &ci, &sc.handle));

        uint32_t imgCount = 0;
        XR_CHECK(xrEnumerateSwapchainImages(sc.handle, 0, &imgCount, nullptr));
        std::vector<XrSwapchainImageVulkanKHR> vkImgs(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(sc.handle, imgCount, &imgCount,
            (XrSwapchainImageBaseHeader*)vkImgs.data()));

        sc.images.resize(imgCount);
        sc.views.resize(imgCount);
        sc.framebuffers.resize(imgCount);
        for (uint32_t i = 0; i < imgCount; i++) {
            sc.images[i] = vkImgs[i].image;

            VkImageViewCreateInfo ivi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            ivi.image = sc.images[i];
            ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivi.format = (VkFormat)format;
            ivi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(app.device, &ivi, nullptr, &sc.views[i]));

            VkFramebufferCreateInfo fbi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbi.renderPass = app.renderPass;
            fbi.attachmentCount = 1;
            fbi.pAttachments = &sc.views[i];
            fbi.width = sc.width;
            fbi.height = sc.height;
            fbi.layers = 1;
            VK_CHECK(vkCreateFramebuffer(app.device, &fbi, nullptr, &sc.framebuffers[i]));
        }
        LOG_INFO("Eye %u swapchain: %ux%u, %u images", eye, sc.width, sc.height, imgCount);
    }
    return true;
}

// ============================================================================
// V1: video texture upload + sampling pipeline
// ============================================================================

static uint32_t FindMemoryType(App& app, uint32_t typeBits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(app.physDevice, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

static std::vector<char> LoadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { LOG_ERROR("cannot open %s", path.c_str()); return {}; }
    std::vector<char> buf((size_t)f.tellg());
    f.seekg(0);
    f.read(buf.data(), (std::streamsize)buf.size());
    return buf;
}

static VkShaderModule LoadShader(App& app, const std::string& path)
{
    std::vector<char> code = LoadFile(path);
    if (code.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(app.device, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

static bool CreatePipeline(App& app, const std::string& shaderDir)
{
    // Descriptor set layout: one combined image sampler (the video texture).
    VkDescriptorSetLayoutBinding b = {};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1;
    dslci.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(app.device, &dslci, nullptr, &app.dsLayout));

    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(app.device, &dpci, nullptr, &app.dsPool));

    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = app.dsPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &app.dsLayout;
    VK_CHECK(vkAllocateDescriptorSets(app.device, &dsai, &app.dset));

    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &app.dsLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(app.device, &plci, nullptr, &app.pipeLayout));

    VkShaderModule vs = LoadShader(app, shaderDir + "/fullscreen.vert.spv");
    VkShaderModule fs = LoadShader(app, shaderDir + "/sample.frag.spv");
    if (!vs || !fs) { LOG_ERROR("shader load failed (dir %s)", shaderDir.c_str()); return false; }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = app.pipeLayout;
    gp.renderPass = app.renderPass;
    gp.subpass = 0;
    VkResult pr = vkCreateGraphicsPipelines(app.device, VK_NULL_HANDLE, 1, &gp, nullptr, &app.pipeline);
    vkDestroyShaderModule(app.device, vs, nullptr);
    vkDestroyShaderModule(app.device, fs, nullptr);
    if (pr != VK_SUCCESS) { LOG_ERROR("pipeline creation failed (%d)", (int)pr); return false; }

    // Sampler (shared; clamp so the SBS half doesn't wrap).
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 1.0f;
    VK_CHECK(vkCreateSampler(app.device, &sci, nullptr, &app.sampler));

    LOG_INFO("Sampling pipeline created");
    return true;
}

// Create/resize the video texture + staging buffer to match the decoded frame.
static bool EnsureVideoTexture(App& app, uint32_t w, uint32_t h)
{
    if (app.videoImage != VK_NULL_HANDLE && app.videoW == w && app.videoH == h) return true;

    vkDeviceWaitIdle(app.device);
    if (app.videoView)  { vkDestroyImageView(app.device, app.videoView, nullptr); app.videoView = VK_NULL_HANDLE; }
    if (app.videoImage) { vkDestroyImage(app.device, app.videoImage, nullptr); app.videoImage = VK_NULL_HANDLE; }
    if (app.videoMem)   { vkFreeMemory(app.device, app.videoMem, nullptr); app.videoMem = VK_NULL_HANDLE; }

    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(app.device, &ici, nullptr, &app.videoImage));

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(app.device, app.videoImage, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = FindMemoryType(app, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(app.device, &mai, nullptr, &app.videoMem));
    VK_CHECK(vkBindImageMemory(app.device, app.videoImage, app.videoMem, 0));

    VkImageViewCreateInfo ivi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivi.image = app.videoImage;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(app.device, &ivi, nullptr, &app.videoView));

    // Staging buffer sized to the frame.
    VkDeviceSize need = (VkDeviceSize)w * h * 4;
    if (need > app.stagingSize) {
        if (app.staging)    { vkDestroyBuffer(app.device, app.staging, nullptr); }
        if (app.stagingMem) { vkUnmapMemory(app.device, app.stagingMem); vkFreeMemory(app.device, app.stagingMem, nullptr); }
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = need;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(app.device, &bci, nullptr, &app.staging));
        VkMemoryRequirements bmr;
        vkGetBufferMemoryRequirements(app.device, app.staging, &bmr);
        VkMemoryAllocateInfo bmai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bmai.allocationSize = bmr.size;
        bmai.memoryTypeIndex = FindMemoryType(app, bmr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(app.device, &bmai, nullptr, &app.stagingMem));
        VK_CHECK(vkBindBufferMemory(app.device, app.staging, app.stagingMem, 0));
        VK_CHECK(vkMapMemory(app.device, app.stagingMem, 0, need, 0, &app.stagingMapped));
        app.stagingSize = need;
    }

    app.videoW = w;
    app.videoH = h;
    app.videoLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    app.descriptorWritten = false;
    LOG_INFO("Video texture %ux%u", w, h);
    return true;
}

// Copy the decoded RGBA frame into the device texture; leaves it SHADER_READ_ONLY.
static bool UploadVideoFrame(App& app, const uint8_t* rgba, uint32_t w, uint32_t h)
{
    if (!EnsureVideoTexture(app, w, h)) return false;
    memcpy(app.stagingMapped, rgba, (size_t)w * h * 4);

    vkResetFences(app.device, 1, &app.fence);
    VK_CHECK(vkResetCommandBuffer(app.cmdBuffer, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(app.cmdBuffer, &bi));

    auto barrier = [&](VkImageLayout from, VkImageLayout to,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = app.videoImage;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(app.cmdBuffer, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy cp = {};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(app.cmdBuffer, app.staging, app.videoImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);

    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    VK_CHECK(vkEndCommandBuffer(app.cmdBuffer));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &app.cmdBuffer;
    VK_CHECK(vkQueueSubmit(app.queue, 1, &si, app.fence));
    vkWaitForFences(app.device, 1, &app.fence, VK_TRUE, UINT64_MAX);
    app.videoLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (!app.descriptorWritten) {
        VkDescriptorImageInfo dii = {app.sampler, app.videoView,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w2 = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w2.dstSet = app.dset;
        w2.dstBinding = 0;
        w2.descriptorCount = 1;
        w2.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w2.pImageInfo = &dii;
        vkUpdateDescriptorSets(app.device, 1, &w2, 0, nullptr);
        app.descriptorWritten = true;
    }
    return true;
}

// One-shot readback of a rendered swapchain image to /tmp (FRAME_CLIENT_DUMP).
// Gold-standard visual check: DisplayXR's atlas trigger doesn't catch our
// two-swapchain layout, so read the actual composited eye back ourselves.
// The image is COLOR_ATTACHMENT_OPTIMAL here (render-pass finalLayout); restore
// it before the caller releases the swapchain image to the runtime.
static void DumpSwapchainImage(App& app, Swapchain& sc, uint32_t imageIndex)
{
    static bool done = false;
    if (done || getenv("FRAME_CLIENT_DUMP") == nullptr) return;
    done = true;

    VkDeviceSize sz = (VkDeviceSize)sc.width * sc.height * 4;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = sz;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(app.device, &bci, nullptr, &buf) != VK_SUCCESS) return;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(app.device, buf, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = FindMemoryType(app, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(app.device, &mai, nullptr, &mem) != VK_SUCCESS) { vkDestroyBuffer(app.device, buf, nullptr); return; }
    vkBindBufferMemory(app.device, buf, mem, 0);

    vkResetFences(app.device, 1, &app.fence);
    vkResetCommandBuffer(app.cmdBuffer, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(app.cmdBuffer, &bi);
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = sc.images[imageIndex];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = sa; b.dstAccessMask = da;
        vkCmdPipelineBarrier(app.cmdBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy cp = {};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {sc.width, sc.height, 1};
    vkCmdCopyImageToBuffer(app.cmdBuffer, sc.images[imageIndex],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    vkEndCommandBuffer(app.cmdBuffer);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &app.cmdBuffer;
    vkQueueSubmit(app.queue, 1, &si, app.fence);
    vkWaitForFences(app.device, 1, &app.fence, VK_TRUE, UINT64_MAX);

    if (vkMapMemory(app.device, mem, 0, sz, 0, &mapped) == VK_SUCCESS) {
        FILE* f = fopen("/tmp/frame_client_eye0.rgba", "wb");
        if (f) { fwrite(mapped, 1, (size_t)sz, f); fclose(f);
            LOG_INFO("dumped rendered eye0 %ux%u to /tmp/frame_client_eye0.rgba", sc.width, sc.height); }
        vkUnmapMemory(app.device, mem);
    }
    vkDestroyBuffer(app.device, buf, nullptr);
    vkFreeMemory(app.device, mem, nullptr);
}

// ============================================================================
// Render one eye: sample this eye's half of the (SBS) video onto the panel
// ============================================================================

static bool RenderEye(App& app, uint32_t eye, uint32_t imageIndex)
{
    Swapchain& sc = app.swapchains[eye];

    vkResetFences(app.device, 1, &app.fence);
    VK_CHECK(vkResetCommandBuffer(app.cmdBuffer, 0));

    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(app.cmdBuffer, &bi));

    VkClearValue clear = {};
    clear.color = {{0.02f, 0.02f, 0.03f, 1.0f}};
    VkRenderPassBeginInfo rpb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpb.renderPass = app.renderPass;
    rpb.framebuffer = sc.framebuffers[imageIndex];
    rpb.renderArea.extent = {sc.width, sc.height};
    rpb.clearValueCount = 1;
    rpb.pClearValues = &clear;
    vkCmdBeginRenderPass(app.cmdBuffer, &rpb, VK_SUBPASS_CONTENTS_INLINE);

    if (app.videoImage != VK_NULL_HANDLE && app.descriptorWritten) {
        VkViewport vpst = {0, 0, (float)sc.width, (float)sc.height, 0.0f, 1.0f};
        VkRect2D scst = {{0, 0}, {sc.width, sc.height}};
        vkCmdSetViewport(app.cmdBuffer, 0, 1, &vpst);
        vkCmdSetScissor(app.cmdBuffer, 0, 1, &scst);
        vkCmdBindPipeline(app.cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, app.pipeline);
        vkCmdBindDescriptorSets(app.cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                app.pipeLayout, 0, 1, &app.dset, 0, nullptr);
        // This eye's half of the SBS frame, plus foveation params (if the
        // server foveated-encoded, the shader un-warps; else it's a plain SBS
        // sample). eye 0 = left half [0,0.5], eye 1 = right half [0.5,1].
        PushConstants pc = {};
        pc.sourceMin[0] = (eye == 0) ? 0.0f : 0.5f; pc.sourceMin[1] = 0.0f;
        pc.sourceMax[0] = (eye == 0) ? 0.5f : 1.0f; pc.sourceMax[1] = 1.0f;
        const StreamConnection::Foveation& fv = g_stream.FoveationParams();
        pc.foveated = (g_streamMode && fv.enabled) ? 1 : 0;
        pc.centerSize[0] = fv.centerSize[0]; pc.centerSize[1] = fv.centerSize[1];
        pc.centerShift[0] = fv.centerShift[0]; pc.centerShift[1] = fv.centerShift[1];
        pc.edgeRatio[0] = fv.edgeRatio[0]; pc.edgeRatio[1] = fv.edgeRatio[1];
        pc.eyeSizeRatio[0] = fv.eyeSizeRatio[0]; pc.eyeSizeRatio[1] = fv.eyeSizeRatio[1];
        vkCmdPushConstants(app.cmdBuffer, app.pipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(app.cmdBuffer, 3, 1, 0, 0);
    }

    vkCmdEndRenderPass(app.cmdBuffer);
    VK_CHECK(vkEndCommandBuffer(app.cmdBuffer));

    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &app.cmdBuffer;
    VK_CHECK(vkQueueSubmit(app.queue, 1, &si, app.fence));
    vkWaitForFences(app.device, 1, &app.fence, VK_TRUE, UINT64_MAX);
    return true;
}

// ============================================================================
// Event loop
// ============================================================================

// Ask the runtime for its highest display refresh rate (V6). oxrsys also
// honours a client's requested refresh via ClientConnect; this covers the
// on-device path where the runtime owns the panel.
static void RequestMaxRefreshRate(App& app)
{
    if (!app.hasRefreshRate) return;
    PFN_xrEnumerateDisplayRefreshRatesFB pfnEnum = nullptr;
    PFN_xrRequestDisplayRefreshRateFB pfnReq = nullptr;
    xrGetInstanceProcAddr(app.instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)&pfnEnum);
    xrGetInstanceProcAddr(app.instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*)&pfnReq);
    if (!pfnEnum || !pfnReq) return;
    uint32_t count = 0;
    if (XR_FAILED(pfnEnum(app.session, 0, &count, nullptr)) || count == 0) return;
    std::vector<float> rates(count);
    if (XR_FAILED(pfnEnum(app.session, count, &count, rates.data()))) return;
    float best = 0;
    for (float r : rates) if (r > best) best = r;
    if (best <= 0) return;
    XrResult r = pfnReq(app.session, best);
    if (XR_SUCCEEDED(r))
        LOG_INFO("display refresh rate set to %.0f Hz", best);
    else
        LOG_INFO("display refresh rate change not supported by this runtime (%.0f Hz requested)", best);
}

static void PollEvents(App& app)
{
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(app.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* ssc = (XrEventDataSessionStateChanged*)&ev;
            app.sessionState = ssc->state;
            LOG_INFO("Session state -> %d", (int)ssc->state);
            if (ssc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(app.session, &bi))) {
                    app.sessionRunning = true;
                    LOG_INFO("Session started");
                    RequestMaxRefreshRate(app);
                }
            } else if (ssc->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(app.session);
                app.sessionRunning = false;
            } else if (ssc->state == XR_SESSION_STATE_EXITING ||
                       ssc->state == XR_SESSION_STATE_LOSS_PENDING) {
                app.exitRequested = true;
            }
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

static void RenderFrame(App& app)
{
    XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(app.session, &wi, &fs))) return;

    XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(app.session, &bi))) return;

    std::vector<XrCompositionLayerProjectionView> projViews(VIEW_COUNT,
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    bool submitLayer = false;

    if (fs.shouldRender) {
        XrView views[VIEW_COUNT] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        XrViewState vstate = {XR_TYPE_VIEW_STATE};
        XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = app.stageSpace;
        uint32_t got = 0;
        if (XR_SUCCEEDED(xrLocateViews(app.session, &li, &vstate, VIEW_COUNT, &got, views)) &&
            got == VIEW_COUNT &&
            (vstate.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
            (vstate.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {

            // Head-tracking proof: log the pose periodically.
            static int fc = 0;
            if (fc++ % 120 == 0) {
                auto& p = views[0].pose.position;
                LOG_INFO("head view[0] pos=(%.3f, %.3f, %.3f)", p.x, p.y, p.z);
            }

            // Ship head + controllers + hands upstream so the server renders
            // for this pose and the game receives the player's input.
            if (g_streamMode) {
                oxr::protocol::TrackingPacket pkt{};
                pkt.timestampNs = fs.predictedDisplayTime;

                // Head-center pose (VIEW space), not the left eye — the server
                // applies its own IPD to derive the stereo pair, so a half-IPD
                // offset here would skew what it renders. Fall back to view[0].
                XrPosef head = views[0].pose;
                XrSpaceLocation hl = {XR_TYPE_SPACE_LOCATION};
                if (app.viewSpace &&
                    XR_SUCCEEDED(xrLocateSpace(app.viewSpace, app.stageSpace,
                                               fs.predictedDisplayTime, &hl)) &&
                    (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                    (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                    head = hl.pose;
                }
                pkt.headPosition[0] = head.position.x;
                pkt.headPosition[1] = head.position.y;
                pkt.headPosition[2] = head.position.z;
                pkt.headOrientation[0] = head.orientation.x;
                pkt.headOrientation[1] = head.orientation.y;
                pkt.headOrientation[2] = head.orientation.z;
                pkt.headOrientation[3] = head.orientation.w;
                // Left-eye FOV (radians, OpenXR-signed). The server normalizes gaze against this
                // to place the foveal centre; without it it falls back to a 45 degree guess.
                pkt.eyeFov[0] = views[0].fov.angleLeft;
                pkt.eyeFov[1] = views[0].fov.angleRight;
                pkt.eyeFov[2] = views[0].fov.angleUp;
                pkt.eyeFov[3] = views[0].fov.angleDown;
                g_xrInput.Update(fs.predictedDisplayTime, pkt);
                g_stream.SendTrackingPacket(pkt);

                // On a decode error (lost reference frame), ask for a keyframe —
                // rate-limited to at most ~2/s so an error burst doesn't flood
                // the server before the fresh IDR arrives.
                if (g_decoder.TakeNeedKeyframe()) {
                    static auto lastReq = std::chrono::steady_clock::time_point{};
                    auto now = std::chrono::steady_clock::now();
                    if (now - lastReq >= std::chrono::milliseconds(500)) {
                        g_stream.SendKeyframeRequest(oxr::protocol::KEYFRAME_REASON_DECODE_STALL, 0);
                        lastReq = now;
                        LOG_INFO("decode error — requested keyframe");
                    }
                }

                static int ic = 0;
                if (ic++ % 120 == 0) {
                    float g[3];
                    bool haveGaze = g_xrInput.GazeDirection(g);
                    LOG_INFO("input: trackingFlags=0x%x buttons=0x%x gaze=%s",
                             pkt.trackingFlags, pkt.buttonState,
                             haveGaze ? "tracked" : "none");
                }
            }

            // Decode + upload the newest frame if one arrived. If not, keep the
            // last texture — we still re-present it, reprojected (repaint).
            int fw = 0, fh = 0;
            int64_t framePtsUs = 0;
            bool haveNew = g_streamMode ? g_decoder.TakeLatestRGBA(g_frameBuf, fw, fh, &framePtsUs)
                                        : g_decoder.NextFrameRGBA(g_frameBuf, fw, fh);
            // Pose and foveation centre for the frame on screen, cached across repaints of the
            // same texture. Matched by presentation time: the latest received metadata leads
            // the displayed frame by the decode pipeline depth, and a moving foveation centre
            // applied off-by-N stretches the periphery.
            static float displayedPos[3] = {0, 0, 0};
            static float displayedOri[4] = {0, 0, 0, 1};
            static bool displayedPoseValid = false;
            if (haveNew && fw > 0 && fh > 0) {
                UploadVideoFrame(app, g_frameBuf.data(), (uint32_t)fw, (uint32_t)fh);
                if (g_streamMode &&
                    g_stream.RenderPoseForFrame(framePtsUs, displayedPos, displayedOri)) {
                    displayedPoseValid = true;
                }
                static bool dumped = false;
                if (!dumped && getenv("FRAME_CLIENT_DUMP")) {
                    FILE* f = fopen("/tmp/frame_client_decoded.rgba", "wb");
                    if (f) { fwrite(g_frameBuf.data(), 1, g_frameBuf.size(), f); fclose(f);
                        LOG_INFO("dumped decoded frame %dx%d to /tmp/frame_client_decoded.rgba", fw, fh); }
                    dumped = true;
                }
            }

            // Async timewarp: submit the layer with the pose the SERVER rendered
            // for, so the runtime compositor reprojects the (possibly stale) frame
            // to the actual display pose. Falls back to the current pose (file
            // mode, or before the first render-pose packet arrives).
            XrPosef layerPose[VIEW_COUNT];
            float rpPos[3], rpOri[4];
            bool haveRenderPose = false;
            if (g_streamMode) {
                if (displayedPoseValid) {
                    memcpy(rpPos, displayedPos, sizeof(rpPos));
                    memcpy(rpOri, displayedOri, sizeof(rpOri));
                    haveRenderPose = true;
                } else {
                    // No frame-matched pose held yet (startup, metadata loss): the newest
                    // received pose is still a better timewarp anchor than none.
                    haveRenderPose = g_stream.LatestRenderPose(rpPos, rpOri);
                }
            }
            if (g_streamMode) {
                static int rc = 0;
                if (rc++ % 120 == 0)
                    LOG_INFO("timewarp: render-pose %s", haveRenderPose ? "in use (compositor reprojects)" : "not yet received");
            }
            for (uint32_t eye = 0; eye < VIEW_COUNT; eye++) {
                if (haveRenderPose) {
                    layerPose[eye].position = {rpPos[0], rpPos[1], rpPos[2]};
                    layerPose[eye].orientation = {rpOri[0], rpOri[1], rpOri[2], rpOri[3]};
                } else {
                    layerPose[eye] = views[eye].pose;
                }
            }

            // Only present once we have real content (a texture).
            auto compositorT0 = std::chrono::steady_clock::now();
            if (app.videoImage != VK_NULL_HANDLE && app.descriptorWritten) {
                for (uint32_t eye = 0; eye < VIEW_COUNT; eye++) {
                    Swapchain& sc = app.swapchains[eye];
                    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    uint32_t idx = 0;
                    if (XR_FAILED(xrAcquireSwapchainImage(sc.handle, &ai, &idx))) break;
                    XrSwapchainImageWaitInfo wii = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    wii.timeout = XR_INFINITE_DURATION;
                    xrWaitSwapchainImage(sc.handle, &wii);

                    RenderEye(app, eye, idx);
                    if (eye == 0) DumpSwapchainImage(app, sc, idx);

                    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage(sc.handle, &ri);

                    projViews[eye].pose = layerPose[eye];
                    projViews[eye].fov = views[eye].fov;
                    projViews[eye].subImage.swapchain = sc.handle;
                    projViews[eye].subImage.imageRect.offset = {0, 0};
                    projViews[eye].subImage.imageRect.extent = {(int32_t)sc.width, (int32_t)sc.height};
                    projViews[eye].subImage.imageArrayIndex = 0;
                }
                layer.space = app.stageSpace;
                layer.viewCount = VIEW_COUNT;
                layer.views = projViews.data();
                submitLayer = true;

                // Feed oxrsys's adaptive bitrate: report client latencies ~1/s.
                if (g_streamMode) {
                    float compMs = (float)std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - compositorT0).count();
                    float decMs = (float)g_decoder.LastDecodeMs();
                    static int lc = 0;
                    if (lc++ % 72 == 0)
                        g_stream.SendLatencyReport(decMs, compMs, decMs + compMs, 0);
                }
            }
        }
    }

    const XrCompositionLayerBaseHeader* layers[] = {(XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = submitLayer ? 1 : 0;
    ei.layers = submitLayer ? layers : nullptr;
    xrEndFrame(app.session, &ei);
}

// ============================================================================
// Cleanup + main
// ============================================================================

static void Destroy(App& app)
{
    if (app.device) vkDeviceWaitIdle(app.device);
    g_xrInput.Destroy();
    g_stream.Disconnect();
    g_decoder.Close();

    // OpenXR objects FIRST. xrDestroySession/xrDestroyInstance make the runtime
    // release its use of our VkDevice and drain any in-flight compositor present
    // (on macOS, DisplayXR presents through MoltenVK/CAMetalDrawable). Destroying
    // the VkDevice before the session lets a pending present callback fire against
    // a freed MoltenVK device — MVKDevice locks a destroyed mutex and aborts.
    for (auto& sc : app.swapchains) {
        for (auto fb : sc.framebuffers) if (fb) vkDestroyFramebuffer(app.device, fb, nullptr);
        for (auto v : sc.views) if (v) vkDestroyImageView(app.device, v, nullptr);
        if (sc.handle) xrDestroySwapchain(sc.handle);
    }
    if (app.viewSpace) xrDestroySpace(app.viewSpace);
    if (app.stageSpace) xrDestroySpace(app.stageSpace);
    if (app.session) xrDestroySession(app.session);
    if (app.instance) xrDestroyInstance(app.instance);

    // Now our own Vulkan objects, device last.
    if (app.pipeline)   vkDestroyPipeline(app.device, app.pipeline, nullptr);
    if (app.pipeLayout) vkDestroyPipelineLayout(app.device, app.pipeLayout, nullptr);
    if (app.dsPool)     vkDestroyDescriptorPool(app.device, app.dsPool, nullptr);
    if (app.dsLayout)   vkDestroyDescriptorSetLayout(app.device, app.dsLayout, nullptr);
    if (app.sampler)    vkDestroySampler(app.device, app.sampler, nullptr);
    if (app.videoView)  vkDestroyImageView(app.device, app.videoView, nullptr);
    if (app.videoImage) vkDestroyImage(app.device, app.videoImage, nullptr);
    if (app.videoMem)   vkFreeMemory(app.device, app.videoMem, nullptr);
    if (app.staging)    vkDestroyBuffer(app.device, app.staging, nullptr);
    if (app.stagingMem) { vkUnmapMemory(app.device, app.stagingMem); vkFreeMemory(app.device, app.stagingMem, nullptr); }
    if (app.renderPass) vkDestroyRenderPass(app.device, app.renderPass, nullptr);
    if (app.fence) vkDestroyFence(app.device, app.fence, nullptr);
    if (app.cmdPool) vkDestroyCommandPool(app.device, app.cmdPool, nullptr);
    if (app.device) vkDestroyDevice(app.device, nullptr);
    if (app.vkInstance) vkDestroyInstance(app.vkInstance, nullptr);
}

int main(int argc, char** argv)
{
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    LOG_INFO("=== Frame VR client — V1: decode a file ===");

    // Mode: stream from an oxrsys server (FRAME_CLIENT_STREAM=1 or --stream),
    // else decode a file (arg 1 / FRAME_CLIENT_VIDEO / bundled test clip).
    g_streamMode = getenv("FRAME_CLIENT_STREAM") != nullptr ||
                   (argc > 1 && strcmp(argv[1], "--stream") == 0);
    if (!g_streamMode) {
        if (argc > 1) g_videoPath = argv[1];
        else if (const char* e = getenv("FRAME_CLIENT_VIDEO")) g_videoPath = e;
        else g_videoPath = "assets/stereo_test.hevc";
    }

    // Shader dir: SHADER_DIR built in, or ./shaders as a fallback.
    std::string shaderDir =
#ifdef FRAME_CLIENT_SHADER_DIR
        FRAME_CLIENT_SHADER_DIR;
#else
        "shaders";
#endif

    App app;
    if (!CreateInstance(app) || !GetSystem(app) || !CreateVulkan(app) ||
        !CreateSession(app) || !CreateSwapchains(app) ||
        !CreatePipeline(app, shaderDir)) {
        LOG_ERROR("Init failed");
        Destroy(app);
        return 1;
    }

    g_xrInput.Init(app.instance, app.session, app.stageSpace, app.hasHandTracking, app.hasEyeGaze);

    if (g_streamMode) {
        if (!g_decoder.OpenStream(VideoDecoder::Codec::H265)) {
            LOG_ERROR("stream decoder open failed"); Destroy(app); return 1;
        }
        LOG_INFO("Discovering oxrsys server...");
        if (!g_stream.Connect(&g_decoder)) {
            LOG_ERROR("Could not connect to an oxrsys server"); Destroy(app); return 1;
        }
    } else if (!g_decoder.Open(g_videoPath)) {
        LOG_ERROR("Cannot open video %s", g_videoPath.c_str());
        Destroy(app);
        return 1;
    }

    LOG_INFO("Entering frame loop (Ctrl-C to quit)");
    bool exitAsked = false;
    while (!app.exitRequested) {
        PollEvents(app);
        // On Ctrl-C, ask the runtime to tear the session down cleanly (drives
        // it through STOPPING -> EXITING) instead of killing it mid-frame.
        if (!g_running && !exitAsked && app.session) {
            xrRequestExitSession(app.session);
            exitAsked = true;
        }
        if (!app.sessionRunning) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        RenderFrame(app);
    }

    LOG_INFO("Shutting down");
    Destroy(app);
    return 0;
}
