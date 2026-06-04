// SPDX-License-Identifier: MPL-2.0

#include "Swapchain.h"
#include "Runtime.h"
#include "VulkanDispatch.h"

#include <spdlog/spdlog.h>
#include <vulkan/vulkan.h>

#include <algorithm>

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
    : device_(metalDevice), graphicsApi_(GraphicsApi::Metal)
{
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
                     const VulkanGraphicsContext& vulkanContext,
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

void Swapchain::InitMetal(void* /*metalDevice*/, const XrSwapchainCreateInfo* /*createInfo*/)
{
    spdlog::error("OXRSys: InitMetal called in a non-Apple runtime build");
}

void Swapchain::InitVulkan(void* /*metalDevice*/, const VulkanGraphicsContext& vulkanContext,
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
    vkDevice_ = vulkanContext.device;
    graphicsApi_ = GraphicsApi::Vulkan;

    VkDevice device = reinterpret_cast<VkDevice>(vulkanContext.device);
    VkPhysicalDevice physDevice = reinterpret_cast<VkPhysicalDevice>(vulkanContext.physicalDevice);
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

Swapchain::~Swapchain()
{
    if (graphicsApi_ == GraphicsApi::Vulkan && vkDevice_ != nullptr)
    {
        VkDevice device = reinterpret_cast<VkDevice>(vkDevice_);
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
    if (graphicsApi_ != GraphicsApi::Vulkan)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    auto* vulkanImages = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
    for (uint32_t i = 0; i < imageCount_; i++)
    {
        vulkanImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        vulkanImages[i].next = nullptr;
        vulkanImages[i].image = reinterpret_cast<VkImage>(vkImages_[i]);
    }

    return XR_SUCCESS;
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
        if (imageStates_[candidateIndex] == ImageState::Available)
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
    if (vkImages_.empty())
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(vkImages_[lastReleasedIndex_]);
}

void* Swapchain::GetLastReleasedTextureSlice(uint32_t arrayIndex) const
{
    if (arrayIndex >= arraySize_)
    {
        spdlog::warn("OXRSys: arrayIndex {} >= arraySize {}", arrayIndex, arraySize_);
        return nullptr;
    }
    return GetLastReleasedTexture();
}

void Swapchain::ReleaseTextureSlice(void* /*textureSlice*/)
{
}

bool Swapchain::HasReleasedImage() const
{
    std::scoped_lock lock(stateMutex_);
    return hasReleasedImage_ && imageStates_[lastReleasedIndex_] == ImageState::Available;
}
