// SPDX-License-Identifier: MPL-2.0

#include "Swapchain.h"
#include "Runtime.h"
#include "VulkanDispatch.h"

#include <spdlog/spdlog.h>
#include <vulkan/vulkan.h>
#if defined(_WIN32)
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <iomanip>
#include <sstream>
#endif

#include <algorithm>
#include <mutex>
#include <thread>

struct VkDeviceFuncs
{
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkGetFenceStatus getFenceStatus = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkInvalidateMappedMemoryRanges invalidateMappedMemoryRanges = nullptr;
};

static VkDeviceFuncs gDeviceFuncs;
static VkDevice gLastLoadedDevice = VK_NULL_HANDLE;
static constexpr uint32_t kBackendSnapshotCount = Swapchain::SwapchainImageCount * 2 + 2;

static std::shared_ptr<void> TryAcquireBackendSnapshotLease(
    const std::shared_ptr<SwapchainSnapshotPoolState>& pool)
{
    if (!pool)
    {
        return {};
    }

    uint32_t current = pool->inUse.load(std::memory_order_acquire);
    while (current < kBackendSnapshotCount)
    {
        if (pool->inUse.compare_exchange_weak(
                current, current + 1, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return std::shared_ptr<void>(pool.get(), [pool](void*) {
                pool->inUse.fetch_sub(1, std::memory_order_acq_rel);
            });
        }
    }
    return {};
}

#if defined(_WIN32)
static std::string HResultString(HRESULT result)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    return stream.str();
}
#endif

static void LoadDeviceFunctions(VkDevice device)
{
    if (device == gLastLoadedDevice && gDeviceFuncs.createImage != nullptr)
    {
        return;
    }
    gLastLoadedDevice = device;

    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = gVulkanDispatch.getDeviceProcAddr;
    if (fpGetDeviceProcAddr == nullptr)
    {
        spdlog::error("OXRSys: vkGetDeviceProcAddr not available");
        return;
    }

    auto get = [device, fpGetDeviceProcAddr](const char* name) {
        return fpGetDeviceProcAddr(device, name);
    };

    gDeviceFuncs.createImage = reinterpret_cast<PFN_vkCreateImage>(get("vkCreateImage"));
    gDeviceFuncs.destroyImage = reinterpret_cast<PFN_vkDestroyImage>(get("vkDestroyImage"));
    gDeviceFuncs.getImageMemoryRequirements =
        reinterpret_cast<PFN_vkGetImageMemoryRequirements>(get("vkGetImageMemoryRequirements"));
    gDeviceFuncs.allocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(get("vkAllocateMemory"));
    gDeviceFuncs.freeMemory = reinterpret_cast<PFN_vkFreeMemory>(get("vkFreeMemory"));
    gDeviceFuncs.bindImageMemory = reinterpret_cast<PFN_vkBindImageMemory>(get("vkBindImageMemory"));
    gDeviceFuncs.createBuffer = reinterpret_cast<PFN_vkCreateBuffer>(get("vkCreateBuffer"));
    gDeviceFuncs.destroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(get("vkDestroyBuffer"));
    gDeviceFuncs.getBufferMemoryRequirements =
        reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get("vkGetBufferMemoryRequirements"));
    gDeviceFuncs.bindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(get("vkBindBufferMemory"));
    gDeviceFuncs.createCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(get("vkCreateCommandPool"));
    gDeviceFuncs.destroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(get("vkDestroyCommandPool"));
    gDeviceFuncs.allocateCommandBuffers =
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(get("vkAllocateCommandBuffers"));
    gDeviceFuncs.beginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get("vkBeginCommandBuffer"));
    gDeviceFuncs.endCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get("vkEndCommandBuffer"));
    gDeviceFuncs.cmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(get("vkCmdPipelineBarrier"));
    gDeviceFuncs.cmdCopyImageToBuffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(get("vkCmdCopyImageToBuffer"));
    gDeviceFuncs.queueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(get("vkQueueSubmit"));
    gDeviceFuncs.createFence = reinterpret_cast<PFN_vkCreateFence>(get("vkCreateFence"));
    gDeviceFuncs.destroyFence = reinterpret_cast<PFN_vkDestroyFence>(get("vkDestroyFence"));
    gDeviceFuncs.waitForFences = reinterpret_cast<PFN_vkWaitForFences>(get("vkWaitForFences"));
    gDeviceFuncs.getFenceStatus = reinterpret_cast<PFN_vkGetFenceStatus>(get("vkGetFenceStatus"));
    gDeviceFuncs.mapMemory = reinterpret_cast<PFN_vkMapMemory>(get("vkMapMemory"));
    gDeviceFuncs.unmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(get("vkUnmapMemory"));
    gDeviceFuncs.invalidateMappedMemoryRanges =
        reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(get("vkInvalidateMappedMemoryRanges"));
}

static bool IsDepthFormat(int64_t format)
{
    VkFormat vkFormat = static_cast<VkFormat>(format);
    return vkFormat == VK_FORMAT_D16_UNORM ||
           vkFormat == VK_FORMAT_D32_SFLOAT ||
           vkFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
           vkFormat == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

#if defined(_WIN32)
static bool IsD3DDepthFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_D16_UNORM ||
           format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
           format == DXGI_FORMAT_D32_FLOAT ||
           format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

static D3D12_RESOURCE_STATES InitialD3D12ResourceState(bool isDepth)
{
    return isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                   : D3D12_RESOURCE_STATE_RENDER_TARGET;
}

static D3D12_RESOURCE_STATES SnapshotD3D12ResourceState(bool isDepth)
{
    return isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                   : D3D12_RESOURCE_STATE_RENDER_TARGET;
}
#endif

static uint32_t FindMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter,
                               VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps = {};
    if (gVulkanDispatch.getPhysicalDeviceMemoryProperties == nullptr)
    {
        spdlog::error("OXRSys: vkGetPhysicalDeviceMemoryProperties not available");
        return 0;
    }

    gVulkanDispatch.getPhysicalDeviceMemoryProperties(physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
    {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }

    return 0;
}

static uint32_t FindMemoryTypeOrInvalid(VkPhysicalDevice physDevice, uint32_t typeFilter,
                                        VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps = {};
    if (gVulkanDispatch.getPhysicalDeviceMemoryProperties == nullptr)
    {
        return UINT32_MAX;
    }

    gVulkanDispatch.getPhysicalDeviceMemoryProperties(physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
    {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static bool RequiredVulkanSnapshotFunctionsAvailable()
{
    return gDeviceFuncs.createBuffer && gDeviceFuncs.destroyBuffer &&
           gDeviceFuncs.getBufferMemoryRequirements && gDeviceFuncs.allocateMemory &&
           gDeviceFuncs.freeMemory && gDeviceFuncs.bindBufferMemory &&
           gDeviceFuncs.createCommandPool && gDeviceFuncs.destroyCommandPool &&
           gDeviceFuncs.allocateCommandBuffers && gDeviceFuncs.beginCommandBuffer &&
           gDeviceFuncs.endCommandBuffer && gDeviceFuncs.cmdPipelineBarrier &&
           gDeviceFuncs.cmdCopyImageToBuffer && gDeviceFuncs.queueSubmit &&
           gDeviceFuncs.createFence && gDeviceFuncs.destroyFence &&
           gDeviceFuncs.waitForFences && gDeviceFuncs.getFenceStatus &&
           gDeviceFuncs.mapMemory && gDeviceFuncs.unmapMemory;
}

Swapchain::Swapchain(void* metalDevice, const XrSwapchainCreateInfo* createInfo)
    : Swapchain(GraphicsContext::Metal(metalDevice), createInfo)
{
}

Swapchain::Swapchain(const GraphicsContext& graphicsContext, const XrSwapchainCreateInfo* createInfo)
    : device_(graphicsContext.metalDevice),
      metalCommandQueue_(graphicsContext.metalCommandQueue),
      graphicsApi_(graphicsContext.api)
{
    if (graphicsContext.api == GraphicsApi::Vulkan)
    {
        InitVulkan(graphicsContext.metalDevice, &graphicsContext.vulkan, createInfo);
        return;
    }
#if defined(_WIN32)
    if (graphicsContext.api == GraphicsApi::D3D11)
    {
        InitD3D11(&graphicsContext.d3d11, createInfo);
        return;
    }
    if (graphicsContext.api == GraphicsApi::D3D12)
    {
        InitD3D12(&graphicsContext.d3d12, createInfo);
        return;
    }
#endif

    if (createInfo != nullptr)
    {
        width_ = createInfo->width;
        height_ = createInfo->height;
        format_ = createInfo->format;
        arraySize_ = createInfo->arraySize > 0 ? createInfo->arraySize : 1;
    }
    initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
    Runtime::Get().RegisterHandle(handle_, this);
    spdlog::error("OXRSys: Metal swapchains are not available in this Linux runtime build");
}

Swapchain::Swapchain(GraphicsApi api, void* metalDevice,
                     const VulkanGraphicsContext* vulkanContext,
                     const XrSwapchainCreateInfo* createInfo)
    : device_(metalDevice), graphicsApi_(api)
{
    if (api == GraphicsApi::Vulkan)
    {
        InitVulkan(metalDevice, vulkanContext, createInfo);
    }
    else
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: Metal swapchains are not available in this Linux runtime build");
    }
}

#if defined(_WIN32)
Swapchain::Swapchain(GraphicsApi api, const D3D11GraphicsContext* d3d11Context,
                     const XrSwapchainCreateInfo* createInfo)
    : graphicsApi_(api)
{
    if (api == GraphicsApi::D3D11)
    {
        InitD3D11(d3d11Context, createInfo);
    }
    else
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: invalid D3D11 swapchain graphics API");
    }
}

Swapchain::Swapchain(GraphicsApi api, const D3D12GraphicsContext* d3d12Context,
                     const XrSwapchainCreateInfo* createInfo)
    : graphicsApi_(api)
{
    if (api == GraphicsApi::D3D12)
    {
        InitD3D12(d3d12Context, createInfo);
    }
    else
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: invalid D3D12 swapchain graphics API");
    }
}
#endif

void Swapchain::InitMetal(void* /*metalDevice*/, const XrSwapchainCreateInfo* /*createInfo*/)
{
    initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
    spdlog::error("OXRSys: InitMetal called in a non-Apple runtime build");
}

void Swapchain::InitMetalStaging(void* /*metalDevice*/)
{
}

void Swapchain::InitVulkan(void* /*metalDevice*/, const VulkanGraphicsContext* vulkanContext,
                           const XrSwapchainCreateInfo* createInfo)
{
    if (createInfo == nullptr)
    {
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    width_ = createInfo->width;
    height_ = createInfo->height;
    format_ = createInfo->format;
    arraySize_ = createInfo->arraySize > 0 ? createInfo->arraySize : 1;
    imageCount_ = (createInfo->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0
        ? 1
        : SwapchainImageCount;
    vulkanContext_ = vulkanContext;
    graphicsApi_ = GraphicsApi::Vulkan;

    if (vulkanContext_ == nullptr || vulkanContext_->device == VK_NULL_HANDLE ||
        vulkanContext_->physicalDevice == VK_NULL_HANDLE)
    {
        spdlog::error("OXRSys: Missing Vulkan graphics context for swapchain creation");
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    VkDevice device = vulkanContext_->device;
    VkPhysicalDevice physDevice = vulkanContext_->physicalDevice;
    LoadDeviceFunctions(device);

    if (gDeviceFuncs.createImage == nullptr ||
        gDeviceFuncs.getImageMemoryRequirements == nullptr ||
        gDeviceFuncs.allocateMemory == nullptr ||
        gDeviceFuncs.bindImageMemory == nullptr)
    {
        spdlog::error("OXRSys: Missing Vulkan device functions for swapchain creation");
        initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    const bool isDepth = IsDepthFormat(format_);

    VkImageCreateInfo imageCI{};
    imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCI.imageType = VK_IMAGE_TYPE_2D;
    imageCI.format = static_cast<VkFormat>(format_);
    imageCI.extent = {width_, height_, 1};
    imageCI.mipLevels = 1;
    imageCI.arrayLayers = arraySize_;
    imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCI.usage = isDepth
        ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
        : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           VK_IMAGE_USAGE_SAMPLED_BIT);
    imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    vkImages_.resize(imageCount_);
    vkMemories_.resize(imageCount_);
    textures_.resize(imageCount_, nullptr);
    imageStates_.assign(imageCount_, ImageState::Available);

    for (uint32_t i = 0; i < imageCount_; i++)
    {
        VkImage image = VK_NULL_HANDLE;
        VkResult result = gDeviceFuncs.createImage(device, &imageCI, nullptr, &image);
        if (result != VK_SUCCESS)
        {
            spdlog::error("OXRSys: vkCreateImage failed with {}", static_cast<int>(result));
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            continue;
        }
        vkImages_[i] = reinterpret_cast<uint64_t>(image);

        VkMemoryRequirements memReqs{};
        gDeviceFuncs.getImageMemoryRequirements(device, image, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(
            physDevice, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VkDeviceMemory memory = VK_NULL_HANDLE;
        result = gDeviceFuncs.allocateMemory(device, &allocInfo, nullptr, &memory);
        if (result != VK_SUCCESS)
        {
            spdlog::error("OXRSys: vkAllocateMemory failed with {}", static_cast<int>(result));
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            continue;
        }
        vkMemories_[i] = reinterpret_cast<uint64_t>(memory);

        result = gDeviceFuncs.bindImageMemory(device, image, memory, 0);
        if (result != VK_SUCCESS)
        {
            spdlog::error("OXRSys: vkBindImageMemory failed with {}", static_cast<int>(result));
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
        }
    }

    for (uint32_t i = 0; i < imageCount_; ++i)
    {
        if (vkImages_[i] == 0 || vkMemories_[i] == 0)
        {
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            break;
        }
    }

    Runtime::Get().RegisterHandle(handle_, this);
    spdlog::info("OXRSys: Vulkan swapchain created {}x{} format={} arraySize={} images={}",
                 width_, height_, format_, arraySize_, imageCount_);
}

#if defined(_WIN32)
void Swapchain::InitD3D11(const D3D11GraphicsContext* d3d11Context,
                          const XrSwapchainCreateInfo* createInfo)
{
    if (createInfo == nullptr)
    {
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    width_ = createInfo->width;
    height_ = createInfo->height;
    format_ = createInfo->format;
    arraySize_ = createInfo->arraySize > 0 ? createInfo->arraySize : 1;
    imageCount_ = (createInfo->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0
        ? 1
        : SwapchainImageCount;
    graphicsApi_ = GraphicsApi::D3D11;
    d3d11Context_ = d3d11Context;

    d3d11Textures_.resize(imageCount_, nullptr);
    textures_.resize(imageCount_, nullptr);
    imageStates_.assign(imageCount_, ImageState::Available);

    if (d3d11Context_ == nullptr || d3d11Context_->device == nullptr)
    {
        spdlog::error("OXRSys: Missing D3D11 graphics context for swapchain creation");
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    const DXGI_FORMAT dxgiFormat = static_cast<DXGI_FORMAT>(format_);
    const bool isDepth = IsD3DDepthFormat(dxgiFormat);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = arraySize_;
    desc.Format = dxgiFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = isDepth
        ? D3D11_BIND_DEPTH_STENCIL
        : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);

    for (uint32_t i = 0; i < imageCount_; ++i)
    {
        ID3D11Texture2D* texture = nullptr;
        HRESULT result = d3d11Context_->device->CreateTexture2D(&desc, nullptr, &texture);
        if (FAILED(result))
        {
            spdlog::error("OXRSys: ID3D11Device::CreateTexture2D failed with {}",
                          HResultString(result));
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            continue;
        }
        d3d11Textures_[i] = texture;
    }

    for (uint32_t i = 0; i < imageCount_; ++i)
    {
        if (d3d11Textures_[i] == nullptr)
        {
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            break;
        }
    }

    Runtime::Get().RegisterHandle(handle_, this);
    spdlog::info("OXRSys: D3D11 swapchain created {}x{} format={} arraySize={} images={}",
                 width_, height_, format_, arraySize_, imageCount_);
}

void Swapchain::InitD3D12(const D3D12GraphicsContext* d3d12Context,
                          const XrSwapchainCreateInfo* createInfo)
{
    if (createInfo == nullptr)
    {
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    width_ = createInfo->width;
    height_ = createInfo->height;
    format_ = createInfo->format;
    arraySize_ = createInfo->arraySize > 0 ? createInfo->arraySize : 1;
    imageCount_ = (createInfo->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0
        ? 1
        : SwapchainImageCount;
    graphicsApi_ = GraphicsApi::D3D12;
    d3d12Context_ = d3d12Context;

    d3d12Resources_.resize(imageCount_, nullptr);
    textures_.resize(imageCount_, nullptr);
    imageStates_.assign(imageCount_, ImageState::Available);

    if (d3d12Context_ == nullptr || d3d12Context_->device == nullptr ||
        d3d12Context_->queue == nullptr)
    {
        spdlog::error("OXRSys: Missing D3D12 graphics context for swapchain creation");
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    const DXGI_FORMAT dxgiFormat = static_cast<DXGI_FORMAT>(format_);
    const bool isDepth = IsD3DDepthFormat(dxgiFormat);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width_;
    desc.Height = height_;
    desc.DepthOrArraySize = static_cast<UINT16>(arraySize_);
    desc.MipLevels = 1;
    desc.Format = dxgiFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                         : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = dxgiFormat;
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 1.0f;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    for (uint32_t i = 0; i < imageCount_; ++i)
    {
        ID3D12Resource* resource = nullptr;
        HRESULT result = d3d12Context_->device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            InitialD3D12ResourceState(isDepth),
            isDepth ? &clearValue : nullptr,
            __uuidof(ID3D12Resource),
            reinterpret_cast<void**>(&resource));
        if (FAILED(result))
        {
            spdlog::error("OXRSys: ID3D12Device::CreateCommittedResource failed with {}",
                          HResultString(result));
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            continue;
        }
        d3d12Resources_[i] = resource;
    }

    for (uint32_t i = 0; i < imageCount_; ++i)
    {
        if (d3d12Resources_[i] == nullptr)
        {
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            break;
        }
    }

    Runtime::Get().RegisterHandle(handle_, this);
    spdlog::info("OXRSys: D3D12 swapchain created {}x{} format={} arraySize={} images={}",
                 width_, height_, format_, arraySize_, imageCount_);
}
#endif

Swapchain::~Swapchain()
{
    if (graphicsApi_ == GraphicsApi::Vulkan && vulkanContext_ != nullptr &&
        vulkanContext_->device != VK_NULL_HANDLE)
    {
        VkDevice device = vulkanContext_->device;
        LoadDeviceFunctions(device);
        for (uint32_t i = 0; i < imageCount_; i++)
        {
            if (i < vkImages_.size() && vkImages_[i] != 0 && gDeviceFuncs.destroyImage != nullptr)
            {
                gDeviceFuncs.destroyImage(device, reinterpret_cast<VkImage>(vkImages_[i]), nullptr);
            }
            if (i < vkMemories_.size() && vkMemories_[i] != 0 && gDeviceFuncs.freeMemory != nullptr)
            {
                gDeviceFuncs.freeMemory(device, reinterpret_cast<VkDeviceMemory>(vkMemories_[i]), nullptr);
            }
        }
    }
#if defined(_WIN32)
    if (graphicsApi_ == GraphicsApi::D3D11)
    {
        for (void* texture : d3d11Textures_)
        {
            if (texture != nullptr)
            {
                static_cast<ID3D11Texture2D*>(texture)->Release();
            }
        }
    }
    if (graphicsApi_ == GraphicsApi::D3D12)
    {
        for (void* resource : d3d12Resources_)
        {
            if (resource != nullptr)
            {
                static_cast<ID3D12Resource*>(resource)->Release();
            }
        }
    }
#endif

    Runtime::Get().RemoveHandle(handle_);
}

XrResult Swapchain::EnumerateImages(uint32_t imageCapacityInput, uint32_t* imageCountOutput,
                                    XrSwapchainImageBaseHeader* images)
{
    if (imageCountOutput == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *imageCountOutput = imageCount_;
    if (imageCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (imageCapacityInput < imageCount_)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    if (graphicsApi_ == GraphicsApi::Vulkan)
    {
        auto* vulkanImages = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
        for (uint32_t i = 0; i < imageCount_; i++)
        {
            vulkanImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
            vulkanImages[i].next = nullptr;
            vulkanImages[i].image = reinterpret_cast<VkImage>(vkImages_[i]);
        }
        return XR_SUCCESS;
    }

#if defined(_WIN32)
    if (graphicsApi_ == GraphicsApi::D3D11)
    {
        auto* d3dImages = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (uint32_t i = 0; i < imageCount_; i++)
        {
            d3dImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
            d3dImages[i].next = nullptr;
            d3dImages[i].texture = i < d3d11Textures_.size()
                ? static_cast<ID3D11Texture2D*>(d3d11Textures_[i])
                : nullptr;
        }
        return XR_SUCCESS;
    }
    if (graphicsApi_ == GraphicsApi::D3D12)
    {
        auto* d3dImages = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
        for (uint32_t i = 0; i < imageCount_; i++)
        {
            d3dImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
            d3dImages[i].next = nullptr;
            d3dImages[i].texture = i < d3d12Resources_.size()
                ? static_cast<ID3D12Resource*>(d3d12Resources_[i])
                : nullptr;
        }
        return XR_SUCCESS;
    }
#endif

    return XR_ERROR_VALIDATION_FAILURE;
}

XrResult Swapchain::AcquireImage(const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index)
{
    if (acquireInfo != nullptr && acquireInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (index == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::scoped_lock lock(stateMutex_);

    if (staticImageAcquired_ || acquiredImageOrder_.size() >= imageCount_)
    {
        spdlog::warn("OXRSys: xrAcquireSwapchainImage rejected (staticAcquired={}, acquired={}, images={})",
                     staticImageAcquired_,
                     acquiredImageOrder_.size(),
                     imageCount_);
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    uint32_t acquiredIndex = imageCount_;
    for (uint32_t i = 0; i < imageCount_; ++i)
    {
        uint32_t candidateIndex = (nextAcquireIndex_ + i) % imageCount_;
        if (imageStates_[candidateIndex] == ImageState::Available)
        {
            acquiredIndex = candidateIndex;
            break;
        }
    }

    if (acquiredIndex == imageCount_)
    {
        spdlog::warn("OXRSys: xrAcquireSwapchainImage rejected (no available image, acquired={}, images={})",
                     acquiredImageOrder_.size(),
                     imageCount_);
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    const bool waitEligible = std::all_of(acquiredImageOrder_.begin(),
                                          acquiredImageOrder_.end(),
                                          [this](uint32_t imageIndex) {
                                              return imageIndex < imageStates_.size() &&
                                                     imageStates_[imageIndex] == ImageState::Waited;
                                          });
    imageStates_[acquiredIndex] = ImageState::Acquired;
    acquiredImageOrder_.push_back(acquiredIndex);
    acquiredImageWaitEligible_.push_back(waitEligible);
    nextAcquireIndex_ = (acquiredIndex + 1) % imageCount_;
    staticImageAcquired_ = imageCount_ == 1;
    *index = acquiredIndex;
    return XR_SUCCESS;
}

XrResult Swapchain::WaitImage(const XrSwapchainImageWaitInfo* waitInfo)
{
    if (waitInfo == nullptr || waitInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::scoped_lock lock(stateMutex_);
    if (acquiredImageOrder_.empty())
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    for (size_t i = 0; i < acquiredImageOrder_.size() && i < acquiredImageWaitEligible_.size(); ++i)
    {
        const uint32_t waitIndex = acquiredImageOrder_[i];
        if (acquiredImageWaitEligible_[i] && waitIndex < imageStates_.size() &&
            imageStates_[waitIndex] == ImageState::Acquired)
        {
            imageStates_[waitIndex] = ImageState::Waited;
            acquiredImageWaitEligible_[i] = false;
            return XR_SUCCESS;
        }
    }

    return XR_ERROR_CALL_ORDER_INVALID;
}

XrResult Swapchain::ReleaseImage(const XrSwapchainImageReleaseInfo* releaseInfo)
{
    if (releaseInfo != nullptr && releaseInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::scoped_lock lock(stateMutex_);
    if (acquiredImageOrder_.empty())
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    uint32_t releaseIndex = acquiredImageOrder_.front();
    if (imageStates_[releaseIndex] != ImageState::Waited)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    lastReleasedIndex_ = releaseIndex;
    imageStates_[releaseIndex] = ImageState::Available;
    hasReleasedImage_ = true;
    acquiredImageOrder_.pop_front();
    acquiredImageWaitEligible_.pop_front();
    if (!acquiredImageWaitEligible_.empty())
    {
        acquiredImageWaitEligible_.front() = true;
    }
    return XR_SUCCESS;
}

void* Swapchain::GetLastReleasedTexture() const
{
    if (graphicsApi_ == GraphicsApi::Vulkan)
    {
        if (vkImages_.empty())
        {
            return nullptr;
        }
        return reinterpret_cast<void*>(vkImages_[lastReleasedIndex_]);
    }
#if defined(_WIN32)
    if (graphicsApi_ == GraphicsApi::D3D11)
    {
        if (d3d11Textures_.empty())
        {
            return nullptr;
        }
        return d3d11Textures_[lastReleasedIndex_];
    }
    if (graphicsApi_ == GraphicsApi::D3D12)
    {
        if (d3d12Resources_.empty())
        {
            return nullptr;
        }
        return d3d12Resources_[lastReleasedIndex_];
    }
#endif
    if (textures_.empty())
    {
        return nullptr;
    }
    return textures_[lastReleasedIndex_];
}

void* Swapchain::GetLastReleasedTextureSlice(uint32_t arrayIndex) const
{
    if (arrayIndex >= arraySize_)
    {
        spdlog::warn("OXRSys: arrayIndex {} >= arraySize {}", arrayIndex, arraySize_);
        return nullptr;
    }

    std::scoped_lock lock(stateMutex_);
    if (!hasReleasedImage_)
    {
        return nullptr;
    }
    if (graphicsApi_ == GraphicsApi::Vulkan)
    {
        if (lastReleasedIndex_ >= vkImages_.size())
        {
            return nullptr;
        }
        return reinterpret_cast<void*>(vkImages_[lastReleasedIndex_]);
    }
#if defined(_WIN32)
    if (graphicsApi_ == GraphicsApi::D3D11)
    {
        if (lastReleasedIndex_ >= d3d11Textures_.size())
        {
            return nullptr;
        }
        return d3d11Textures_[lastReleasedIndex_];
    }
    if (graphicsApi_ == GraphicsApi::D3D12)
    {
        if (lastReleasedIndex_ >= d3d12Resources_.size())
        {
            return nullptr;
        }
        return d3d12Resources_[lastReleasedIndex_];
    }
#endif
    return GetLastReleasedTexture();
}

FrameImageSource Swapchain::GetLastReleasedFrameImageSource(uint32_t arrayIndex) const
{
#ifdef XR_USE_GRAPHICS_API_VULKAN
    if (graphicsApi_ == GraphicsApi::Vulkan)
    {
        return SnapshotVulkanFrameImageSource(arrayIndex);
    }
#endif
#if defined(_WIN32)
    if (graphicsApi_ == GraphicsApi::D3D11)
    {
        return SnapshotD3D11FrameImageSource(arrayIndex);
    }
    if (graphicsApi_ == GraphicsApi::D3D12)
    {
        return SnapshotD3D12FrameImageSource(arrayIndex);
    }
#endif
    return {};
}

void Swapchain::ReleaseTextureSlice(void* /*textureSlice*/)
{
}

bool Swapchain::HasReleasedImage() const
{
    std::scoped_lock lock(stateMutex_);
    return hasReleasedImage_ && imageStates_[lastReleasedIndex_] == ImageState::Available;
}

#ifdef XR_USE_GRAPHICS_API_VULKAN
FrameImageSource Swapchain::SnapshotVulkanFrameImageSource(uint32_t arrayIndex) const
{
    VulkanGraphicsContext context = {};
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    std::shared_ptr<SwapchainSnapshotPoolState> pool;
    {
        std::scoped_lock lock(stateMutex_);
        if (!hasReleasedImage_ || arrayIndex >= arraySize_ ||
            lastReleasedIndex_ >= vkImages_.size() || vulkanContext_ == nullptr)
        {
            return {};
        }
        image = reinterpret_cast<VkImage>(vkImages_[lastReleasedIndex_]);
        context = *vulkanContext_;
        format = static_cast<VkFormat>(format_);
        width = width_;
        height = height_;
        pool = backendSnapshotPool_;
    }

    if (image == VK_NULL_HANDLE || context.device == VK_NULL_HANDLE ||
        context.queue == VK_NULL_HANDLE || IsDepthFormat(format) ||
        width == 0 || height == 0)
    {
        return {};
    }

    std::shared_ptr<void> lease = TryAcquireBackendSnapshotLease(pool);
    if (!lease)
    {
        spdlog::debug("OXRSys: Vulkan snapshot pool is full; streaming frame dropped");
        return {};
    }

    LoadDeviceFunctions(context.device);
    if (!RequiredVulkanSnapshotFunctionsAvailable())
    {
        spdlog::warn("OXRSys: Vulkan snapshot unavailable because required device functions are missing");
        return {};
    }

    auto* source = new VulkanFrameSource();
    source->lifetime = lease;
    source->context = context;
    source->format = format;
    source->width = width;
    source->height = height;
    source->destroyCommandPool = gDeviceFuncs.destroyCommandPool;
    source->destroyBuffer = gDeviceFuncs.destroyBuffer;
    source->freeMemory = gDeviceFuncs.freeMemory;
    source->destroyFence = gDeviceFuncs.destroyFence;
    source->waitForFences = gDeviceFuncs.waitForFences;
    source->getFenceStatus = gDeviceFuncs.getFenceStatus;
    source->mapMemory = gDeviceFuncs.mapMemory;
    source->unmapMemory = gDeviceFuncs.unmapMemory;
    source->invalidateMappedMemoryRanges = gDeviceFuncs.invalidateMappedMemoryRanges;

    const size_t bytesPerPixel = 4;
    const size_t readbackSize = static_cast<size_t>(width) * height * bytesPerPixel;

    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = readbackSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (gDeviceFuncs.createBuffer(context.device, &bufferInfo, nullptr, &source->stagingBuffer) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkMemoryRequirements memoryRequirements = {};
    gDeviceFuncs.getBufferMemoryRequirements(context.device, source->stagingBuffer, &memoryRequirements);
    const uint32_t memoryType = FindMemoryTypeOrInvalid(
        context.physicalDevice,
        memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == UINT32_MAX)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memoryRequirements.size;
    allocInfo.memoryTypeIndex = memoryType;
    if (gDeviceFuncs.allocateMemory(context.device, &allocInfo, nullptr, &source->stagingMemory) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }
    if (gDeviceFuncs.bindBufferMemory(context.device, source->stagingBuffer, source->stagingMemory, 0) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = context.queueFamilyIndex;
    if (gDeviceFuncs.createCommandPool(context.device, &poolInfo, nullptr, &source->commandPool) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo commandInfo = {};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = source->commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    if (gDeviceFuncs.allocateCommandBuffers(context.device, &commandInfo, &commandBuffer) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (gDeviceFuncs.createFence(context.device, &fenceInfo, nullptr, &source->fence) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (gDeviceFuncs.beginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkImageMemoryBarrier toTransfer = {};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = 0;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.baseArrayLayer = arrayIndex;
    toTransfer.subresourceRange.layerCount = 1;

    gDeviceFuncs.cmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = arrayIndex;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    gDeviceFuncs.cmdCopyImageToBuffer(
        commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source->stagingBuffer, 1, &region);

    VkImageMemoryBarrier backToColor = toTransfer;
    backToColor.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    backToColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    backToColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    backToColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    gDeviceFuncs.cmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &backToColor);

    if (gDeviceFuncs.endCommandBuffer(commandBuffer) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    if (gDeviceFuncs.queueSubmit(context.queue, 1, &submitInfo, source->fence) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }
    source->fenceSubmitted = true;
    source->stagingSize = readbackSize;

    FrameImageSource frameSource = {};
    frameSource.api = GraphicsApi::Vulkan;
    frameSource.lifetime = lease;
    frameSource.image = std::shared_ptr<void>(source, [](void* value) {
        ReleaseVulkanFrameSource(static_cast<VulkanFrameSource*>(value));
    });
    return frameSource;
}

void Swapchain::ReleaseVulkanFrameSource(VulkanFrameSource* source)
{
    if (source == nullptr)
    {
        return;
    }

    auto destroy = [](VulkanFrameSource* sourceToDestroy) {
        if (sourceToDestroy->fenceSubmitted && sourceToDestroy->fence != VK_NULL_HANDLE &&
            sourceToDestroy->waitForFences != nullptr)
        {
            sourceToDestroy->waitForFences(
                sourceToDestroy->context.device, 1, &sourceToDestroy->fence, VK_TRUE, UINT64_MAX);
        }
        if (sourceToDestroy->commandPool != VK_NULL_HANDLE &&
            sourceToDestroy->destroyCommandPool != nullptr)
        {
            sourceToDestroy->destroyCommandPool(
                sourceToDestroy->context.device, sourceToDestroy->commandPool, nullptr);
        }
        if (sourceToDestroy->stagingBuffer != VK_NULL_HANDLE &&
            sourceToDestroy->destroyBuffer != nullptr)
        {
            sourceToDestroy->destroyBuffer(
                sourceToDestroy->context.device, sourceToDestroy->stagingBuffer, nullptr);
        }
        if (sourceToDestroy->stagingMemory != VK_NULL_HANDLE &&
            sourceToDestroy->freeMemory != nullptr)
        {
            sourceToDestroy->freeMemory(
                sourceToDestroy->context.device, sourceToDestroy->stagingMemory, nullptr);
        }
        if (sourceToDestroy->fence != VK_NULL_HANDLE &&
            sourceToDestroy->destroyFence != nullptr)
        {
            sourceToDestroy->destroyFence(
                sourceToDestroy->context.device, sourceToDestroy->fence, nullptr);
        }
        delete sourceToDestroy;
    };

    if (source->fenceSubmitted && source->fence != VK_NULL_HANDLE &&
        source->getFenceStatus != nullptr &&
        source->getFenceStatus(source->context.device, source->fence) == VK_NOT_READY)
    {
        std::thread(destroy, source).detach();
        return;
    }
    destroy(source);
}
#endif

#if defined(_WIN32)
FrameImageSource Swapchain::SnapshotD3D11FrameImageSource(uint32_t arrayIndex) const
{
    ID3D11Texture2D* texture = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediateContext = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    std::shared_ptr<SwapchainSnapshotPoolState> pool;
    {
        std::scoped_lock lock(stateMutex_);
        if (!hasReleasedImage_ || arrayIndex >= arraySize_ ||
            lastReleasedIndex_ >= d3d11Textures_.size() || d3d11Context_ == nullptr ||
            d3d11Context_->device == nullptr || d3d11Context_->immediateContext == nullptr ||
            d3d11Textures_[lastReleasedIndex_] == nullptr)
        {
            return {};
        }
        texture = static_cast<ID3D11Texture2D*>(d3d11Textures_[lastReleasedIndex_]);
        texture->AddRef();
        device = d3d11Context_->device;
        device->AddRef();
        immediateContext = d3d11Context_->immediateContext;
        immediateContext->AddRef();
        format = static_cast<DXGI_FORMAT>(format_);
        width = width_;
        height = height_;
        pool = backendSnapshotPool_;
    }

    auto releaseLocals = [&]() {
        if (texture != nullptr)
        {
            texture->Release();
            texture = nullptr;
        }
        if (device != nullptr)
        {
            device->Release();
            device = nullptr;
        }
        if (immediateContext != nullptr)
        {
            immediateContext->Release();
            immediateContext = nullptr;
        }
    };

    if (IsD3DDepthFormat(format) || width == 0 || height == 0)
    {
        releaseLocals();
        return {};
    }

    std::shared_ptr<void> lease = TryAcquireBackendSnapshotLease(pool);
    if (!lease)
    {
        releaseLocals();
        spdlog::debug("OXRSys: D3D11 snapshot pool is full; streaming frame dropped");
        return {};
    }

    auto* source = new D3D11FrameSource();
    source->lifetime = lease;
    source->immediateContext = immediateContext;
    immediateContext = nullptr;
    source->format = format;
    source->width = width;
    source->height = height;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT result = device->CreateTexture2D(&desc, nullptr, &source->stagingTexture);
    if (FAILED(result))
    {
        spdlog::warn("OXRSys: D3D11 staging texture creation failed with {}", HResultString(result));
        releaseLocals();
        ReleaseD3D11FrameSource(source);
        return {};
    }

    const UINT sourceSubresource = D3D11CalcSubresource(0, arrayIndex, 1);
    source->immediateContext->CopySubresourceRegion(
        source->stagingTexture, 0, 0, 0, 0, texture, sourceSubresource, nullptr);
    releaseLocals();

    FrameImageSource frameSource = {};
    frameSource.api = GraphicsApi::D3D11;
    frameSource.lifetime = lease;
    frameSource.image = std::shared_ptr<void>(source, [](void* value) {
        ReleaseD3D11FrameSource(static_cast<D3D11FrameSource*>(value));
    });
    return frameSource;
}

FrameImageSource Swapchain::SnapshotD3D12FrameImageSource(uint32_t arrayIndex) const
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Resource* texture = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    std::shared_ptr<SwapchainSnapshotPoolState> pool;
    {
        std::scoped_lock lock(stateMutex_);
        if (!hasReleasedImage_ || arrayIndex >= arraySize_ ||
            lastReleasedIndex_ >= d3d12Resources_.size() || d3d12Context_ == nullptr ||
            d3d12Context_->device == nullptr || d3d12Context_->queue == nullptr ||
            d3d12Resources_[lastReleasedIndex_] == nullptr)
        {
            return {};
        }
        device = d3d12Context_->device;
        device->AddRef();
        queue = d3d12Context_->queue;
        queue->AddRef();
        texture = static_cast<ID3D12Resource*>(d3d12Resources_[lastReleasedIndex_]);
        texture->AddRef();
        format = static_cast<DXGI_FORMAT>(format_);
        width = width_;
        height = height_;
        pool = backendSnapshotPool_;
    }

    auto releaseLocals = [&]() {
        if (texture != nullptr)
        {
            texture->Release();
            texture = nullptr;
        }
        if (queue != nullptr)
        {
            queue->Release();
            queue = nullptr;
        }
        if (device != nullptr)
        {
            device->Release();
            device = nullptr;
        }
    };

    const bool isDepth = IsD3DDepthFormat(format);
    if (isDepth || width == 0 || height == 0)
    {
        releaseLocals();
        return {};
    }

    std::shared_ptr<void> lease = TryAcquireBackendSnapshotLease(pool);
    if (!lease)
    {
        releaseLocals();
        spdlog::debug("OXRSys: D3D12 snapshot pool is full; streaming frame dropped");
        return {};
    }

    auto* source = new D3D12FrameSource();
    source->lifetime = lease;
    source->sourceTexture = texture;
    texture = nullptr;
    source->format = format;
    source->width = width;
    source->height = height;

    D3D12_RESOURCE_DESC textureDesc = source->sourceTexture->GetDesc();
    const UINT sourceSubresource = arrayIndex * textureDesc.MipLevels;
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    device->GetCopyableFootprints(
        &textureDesc,
        sourceSubresource,
        1,
        0,
        &source->footprint,
        &numRows,
        &rowSizeInBytes,
        &source->totalBytes);
    if (source->totalBytes == 0 || rowSizeInBytes < static_cast<UINT64>(width) * 4)
    {
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC readbackDesc = {};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = source->totalBytes;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT result = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &readbackDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        __uuidof(ID3D12Resource),
        reinterpret_cast<void**>(&source->readbackBuffer));
    if (FAILED(result))
    {
        spdlog::warn("OXRSys: D3D12 readback buffer creation failed with {}", HResultString(result));
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }

    const D3D12_COMMAND_LIST_TYPE listType = queue->GetDesc().Type;
    if (listType != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        listType != D3D12_COMMAND_LIST_TYPE_COPY)
    {
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }
    if (FAILED(device->CreateCommandAllocator(
            listType,
            __uuidof(ID3D12CommandAllocator),
            reinterpret_cast<void**>(&source->commandAllocator))) ||
        FAILED(device->CreateCommandList(
            0,
            listType,
            source->commandAllocator,
            nullptr,
            __uuidof(ID3D12GraphicsCommandList),
            reinterpret_cast<void**>(&source->commandList))) ||
        FAILED(device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            __uuidof(ID3D12Fence),
            reinterpret_cast<void**>(&source->fence))))
    {
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }
    source->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (source->fenceEvent == nullptr)
    {
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }

    D3D12_RESOURCE_BARRIER toCopy = {};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = source->sourceTexture;
    toCopy.Transition.Subresource = sourceSubresource;
    toCopy.Transition.StateBefore = SnapshotD3D12ResourceState(isDepth);
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    source->commandList->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = source->readbackBuffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = source->footprint;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = source->sourceTexture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = sourceSubresource;
    source->commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER backToRender = toCopy;
    backToRender.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    backToRender.Transition.StateAfter = SnapshotD3D12ResourceState(isDepth);
    source->commandList->ResourceBarrier(1, &backToRender);

    if (FAILED(source->commandList->Close()))
    {
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }

    ID3D12CommandList* commandLists[] = {source->commandList};
    queue->ExecuteCommandLists(1, commandLists);
    source->fenceValue = 1;
    if (FAILED(queue->Signal(source->fence, source->fenceValue)))
    {
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }
    source->fenceSubmitted = true;
    releaseLocals();

    FrameImageSource frameSource = {};
    frameSource.api = GraphicsApi::D3D12;
    frameSource.lifetime = lease;
    frameSource.image = std::shared_ptr<void>(source, [](void* value) {
        ReleaseD3D12FrameSource(static_cast<D3D12FrameSource*>(value));
    });
    return frameSource;
}

void Swapchain::ReleaseD3D11FrameSource(D3D11FrameSource* source)
{
    if (source == nullptr)
    {
        return;
    }
    if (source->stagingTexture != nullptr)
    {
        source->stagingTexture->Release();
        source->stagingTexture = nullptr;
    }
    if (source->immediateContext != nullptr)
    {
        source->immediateContext->Release();
        source->immediateContext = nullptr;
    }
    delete source;
}

void Swapchain::ReleaseD3D12FrameSource(D3D12FrameSource* source)
{
    if (source == nullptr)
    {
        return;
    }

    auto destroy = [](D3D12FrameSource* sourceToDestroy) {
        if (sourceToDestroy->fenceSubmitted && sourceToDestroy->fence != nullptr &&
            sourceToDestroy->fence->GetCompletedValue() < sourceToDestroy->fenceValue)
        {
            bool waitingOnEvent = false;
            if (sourceToDestroy->fenceEvent != nullptr &&
                SUCCEEDED(sourceToDestroy->fence->SetEventOnCompletion(
                    sourceToDestroy->fenceValue, sourceToDestroy->fenceEvent)))
            {
                waitingOnEvent = true;
                WaitForSingleObject(sourceToDestroy->fenceEvent, INFINITE);
            }
            while (!waitingOnEvent &&
                   sourceToDestroy->fence->GetCompletedValue() < sourceToDestroy->fenceValue)
            {
                Sleep(1);
            }
        }
        if (sourceToDestroy->commandList != nullptr)
        {
            sourceToDestroy->commandList->Release();
            sourceToDestroy->commandList = nullptr;
        }
        if (sourceToDestroy->commandAllocator != nullptr)
        {
            sourceToDestroy->commandAllocator->Release();
            sourceToDestroy->commandAllocator = nullptr;
        }
        if (sourceToDestroy->readbackBuffer != nullptr)
        {
            sourceToDestroy->readbackBuffer->Release();
            sourceToDestroy->readbackBuffer = nullptr;
        }
        if (sourceToDestroy->sourceTexture != nullptr)
        {
            sourceToDestroy->sourceTexture->Release();
            sourceToDestroy->sourceTexture = nullptr;
        }
        if (sourceToDestroy->fence != nullptr)
        {
            sourceToDestroy->fence->Release();
            sourceToDestroy->fence = nullptr;
        }
        if (sourceToDestroy->fenceEvent != nullptr)
        {
            CloseHandle(sourceToDestroy->fenceEvent);
            sourceToDestroy->fenceEvent = nullptr;
        }
        delete sourceToDestroy;
    };

    if (source->fenceSubmitted && source->fence != nullptr &&
        source->fence->GetCompletedValue() < source->fenceValue)
    {
        std::thread(destroy, source).detach();
        return;
    }
    destroy(source);
}
#endif
