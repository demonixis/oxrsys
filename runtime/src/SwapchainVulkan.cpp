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
#include <unordered_set>

struct VkDeviceFuncs
{
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
};

static VkDeviceFuncs gDeviceFuncs;
static VkDevice gLastLoadedDevice = VK_NULL_HANDLE;
static std::mutex gVulkanFrameSourceMutex;
static std::unordered_set<Swapchain::VulkanFrameSource*> gVulkanFrameSources;
#if defined(_WIN32)
static std::mutex gD3D11FrameSourceMutex;
static std::mutex gD3D12FrameSourceMutex;
static std::unordered_set<Swapchain::D3D11FrameSource*> gD3D11FrameSources;
static std::unordered_set<Swapchain::D3D12FrameSource*> gD3D12FrameSources;

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
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: invalid D3D12 swapchain graphics API");
    }
}
#endif

void Swapchain::InitMetal(void* /*metalDevice*/, const XrSwapchainCreateInfo* /*createInfo*/)
{
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
    imageRetainCounts_.assign(imageCount_, 0);
    textures_.resize(imageCount_, nullptr);
    imageStates_.assign(imageCount_, ImageState::Available);

    for (uint32_t i = 0; i < imageCount_; i++)
    {
        VkImage image = VK_NULL_HANDLE;
        VkResult result = gDeviceFuncs.createImage(device, &imageCI, nullptr, &image);
        if (result != VK_SUCCESS)
        {
            spdlog::error("OXRSys: vkCreateImage failed with {}", static_cast<int>(result));
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
            continue;
        }
        vkMemories_[i] = reinterpret_cast<uint64_t>(memory);

        result = gDeviceFuncs.bindImageMemory(device, image, memory, 0);
        if (result != VK_SUCCESS)
        {
            spdlog::error("OXRSys: vkBindImageMemory failed with {}", static_cast<int>(result));
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
    imageRetainCounts_.assign(imageCount_, 0);
    imageStates_.assign(imageCount_, ImageState::Available);

    if (d3d11Context_ == nullptr || d3d11Context_->device == nullptr)
    {
        spdlog::error("OXRSys: Missing D3D11 graphics context for swapchain creation");
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
            continue;
        }
        d3d11Textures_[i] = texture;
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
    imageRetainCounts_.assign(imageCount_, 0);
    imageStates_.assign(imageCount_, ImageState::Available);

    if (d3d12Context_ == nullptr || d3d12Context_->device == nullptr)
    {
        spdlog::error("OXRSys: Missing D3D12 graphics context for swapchain creation");
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
            D3D12_RESOURCE_STATE_COMMON,
            isDepth ? &clearValue : nullptr,
            __uuidof(ID3D12Resource),
            reinterpret_cast<void**>(&resource));
        if (FAILED(result))
        {
            spdlog::error("OXRSys: ID3D12Device::CreateCommittedResource failed with {}",
                          HResultString(result));
            continue;
        }
        d3d12Resources_[i] = resource;
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
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    uint32_t acquiredIndex = imageCount_;
    for (uint32_t i = 0; i < imageCount_; ++i)
    {
        uint32_t candidateIndex = (nextAcquireIndex_ + i) % imageCount_;
        if (imageStates_[candidateIndex] == ImageState::Available &&
            (candidateIndex >= imageRetainCounts_.size() || imageRetainCounts_[candidateIndex] == 0))
        {
            acquiredIndex = candidateIndex;
            break;
        }
    }

    if (acquiredIndex == imageCount_)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    imageStates_[acquiredIndex] = ImageState::Acquired;
    acquiredImageOrder_.push_back(acquiredIndex);
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

    uint32_t waitIndex = acquiredImageOrder_.front();
    if (imageStates_[waitIndex] != ImageState::Acquired)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    imageStates_[waitIndex] = ImageState::Waited;
    return XR_SUCCESS;
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
    if (graphicsApi_ == GraphicsApi::Vulkan)
    {
        std::scoped_lock lock(stateMutex_);
        if (!hasReleasedImage_ || lastReleasedIndex_ >= vkImages_.size() ||
            lastReleasedIndex_ >= imageRetainCounts_.size() || vulkanContext_ == nullptr)
        {
            return nullptr;
        }

        auto* source = new VulkanFrameSource();
        source->owner = const_cast<Swapchain*>(this);
        source->context = vulkanContext_;
        source->image = reinterpret_cast<VkImage>(vkImages_[lastReleasedIndex_]);
        source->format = static_cast<VkFormat>(format_);
        source->width = width_;
        source->height = height_;
        source->arrayLayer = arrayIndex;
        source->imageIndex = lastReleasedIndex_;
        const_cast<Swapchain*>(this)->RetainImage(lastReleasedIndex_);
        {
            std::lock_guard<std::mutex> sourceLock(gVulkanFrameSourceMutex);
            gVulkanFrameSources.insert(source);
        }
        return source;
    }
#if defined(_WIN32)
    if (graphicsApi_ == GraphicsApi::D3D11)
    {
        std::scoped_lock lock(stateMutex_);
        if (!hasReleasedImage_ || lastReleasedIndex_ >= d3d11Textures_.size() ||
            lastReleasedIndex_ >= imageRetainCounts_.size() || d3d11Context_ == nullptr ||
            d3d11Textures_[lastReleasedIndex_] == nullptr)
        {
            return nullptr;
        }

        auto* source = new D3D11FrameSource();
        source->owner = const_cast<Swapchain*>(this);
        source->context = d3d11Context_;
        source->texture = static_cast<ID3D11Texture2D*>(d3d11Textures_[lastReleasedIndex_]);
        source->texture->AddRef();
        source->format = static_cast<DXGI_FORMAT>(format_);
        source->width = width_;
        source->height = height_;
        source->arrayLayer = arrayIndex;
        source->imageIndex = lastReleasedIndex_;
        const_cast<Swapchain*>(this)->RetainImage(lastReleasedIndex_);
        {
            std::lock_guard<std::mutex> sourceLock(gD3D11FrameSourceMutex);
            gD3D11FrameSources.insert(source);
        }
        return source;
    }
    if (graphicsApi_ == GraphicsApi::D3D12)
    {
        std::scoped_lock lock(stateMutex_);
        if (!hasReleasedImage_ || lastReleasedIndex_ >= d3d12Resources_.size() ||
            lastReleasedIndex_ >= imageRetainCounts_.size() || d3d12Context_ == nullptr ||
            d3d12Resources_[lastReleasedIndex_] == nullptr)
        {
            return nullptr;
        }

        auto* source = new D3D12FrameSource();
        source->owner = const_cast<Swapchain*>(this);
        source->context = d3d12Context_;
        source->texture = static_cast<ID3D12Resource*>(d3d12Resources_[lastReleasedIndex_]);
        source->texture->AddRef();
        source->format = static_cast<DXGI_FORMAT>(format_);
        source->width = width_;
        source->height = height_;
        source->arrayLayer = arrayIndex;
        source->imageIndex = lastReleasedIndex_;
        const_cast<Swapchain*>(this)->RetainImage(lastReleasedIndex_);
        {
            std::lock_guard<std::mutex> sourceLock(gD3D12FrameSourceMutex);
            gD3D12FrameSources.insert(source);
        }
        return source;
    }
#endif
    return GetLastReleasedTexture();
}

FrameImageSource Swapchain::GetLastReleasedFrameImageSource(uint32_t arrayIndex) const
{
    void* imageSource = GetLastReleasedTextureSlice(arrayIndex);
    if (imageSource == nullptr)
    {
        return {};
    }

    FrameImageSource source = {};
    source.api = graphicsApi_;
    source.image = std::shared_ptr<void>(imageSource, [](void* value) {
        Swapchain::ReleaseTextureSlice(value);
    });
    return source;
}

void Swapchain::ReleaseTextureSlice(void* textureSlice)
{
    if (TryReleaseVulkanFrameSource(textureSlice))
    {
        return;
    }
#if defined(_WIN32)
    if (TryReleaseD3D11FrameSource(textureSlice))
    {
        return;
    }
    TryReleaseD3D12FrameSource(textureSlice);
#endif
}

bool Swapchain::HasReleasedImage() const
{
    std::scoped_lock lock(stateMutex_);
    return hasReleasedImage_ && imageStates_[lastReleasedIndex_] == ImageState::Available;
}

void Swapchain::RetainImage(uint32_t imageIndex)
{
    if (imageIndex < imageRetainCounts_.size())
    {
        imageRetainCounts_[imageIndex]++;
    }
}

void Swapchain::ReleaseImageRetention(uint32_t imageIndex)
{
    std::scoped_lock lock(stateMutex_);
    if (imageIndex < imageRetainCounts_.size() && imageRetainCounts_[imageIndex] > 0)
    {
        imageRetainCounts_[imageIndex]--;
    }
}

bool Swapchain::TryReleaseVulkanFrameSource(void* textureSlice)
{
    auto* source = static_cast<VulkanFrameSource*>(textureSlice);
    if (source == nullptr)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(gVulkanFrameSourceMutex);
        auto it = gVulkanFrameSources.find(source);
        if (it == gVulkanFrameSources.end())
        {
            return false;
        }
        gVulkanFrameSources.erase(it);
    }

    if (source->owner != nullptr)
    {
        source->owner->ReleaseImageRetention(source->imageIndex);
    }
    delete source;
    return true;
}

#if defined(_WIN32)
bool Swapchain::TryReleaseD3D11FrameSource(void* textureSlice)
{
    auto* source = static_cast<D3D11FrameSource*>(textureSlice);
    if (source == nullptr)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(gD3D11FrameSourceMutex);
        auto it = gD3D11FrameSources.find(source);
        if (it == gD3D11FrameSources.end())
        {
            return false;
        }
        gD3D11FrameSources.erase(it);
    }

    if (source->owner != nullptr)
    {
        source->owner->ReleaseImageRetention(source->imageIndex);
    }
    if (source->texture != nullptr)
    {
        source->texture->Release();
    }
    delete source;
    return true;
}

bool Swapchain::TryReleaseD3D12FrameSource(void* textureSlice)
{
    auto* source = static_cast<D3D12FrameSource*>(textureSlice);
    if (source == nullptr)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(gD3D12FrameSourceMutex);
        auto it = gD3D12FrameSources.find(source);
        if (it == gD3D12FrameSources.end())
        {
            return false;
        }
        gD3D12FrameSources.erase(it);
    }

    if (source->owner != nullptr)
    {
        source->owner->ReleaseImageRetention(source->imageIndex);
    }
    if (source->texture != nullptr)
    {
        source->texture->Release();
    }
    delete source;
    return true;
}
#endif
