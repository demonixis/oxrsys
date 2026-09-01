// SPDX-License-Identifier: MPL-2.0

#include "Swapchain.h"
#include "Runtime.h"
#include "SwapchainCreateValidation.h"
#include "VulkanDispatch.h"

#include <spdlog/spdlog.h>
#include <vulkan/vulkan.h>
#if defined(__APPLE__)
#include <vulkan/vulkan_metal.h>
#endif

#include <chrono>
#include <limits>
#include <vector>

#include "OpenXRPlatform.h"

#if defined(__APPLE__)
extern "C" void* OxrsysRetainMetalObjectForSwapchain(void* object);
extern "C" void* OxrsysCreateMetalTextureSliceForSwapchain(
    void* object, uint32_t arraySize, uint32_t arrayIndex);
#endif

namespace
{

// Three VideoToolbox buffers may be in flight while the latest-frame queue and
// the swapchain each retain one distinct snapshot. Five slots avoid allocation
// on release without allowing an unbounded backlog.
constexpr size_t kVulkanSnapshotSlotCount = Swapchain::SwapchainImageCount + 2;
constexpr auto kVulkanDestroyDrainTimeout = std::chrono::milliseconds(500);

std::shared_ptr<const VulkanDeviceFunctions> LoadDeviceFunctions(VkDevice device)
{
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = gVulkanDispatch.getDeviceProcAddr;
    if (fpGetDeviceProcAddr == nullptr)
    {
        spdlog::error("OXRSys: vkGetDeviceProcAddr not available");
        return {};
    }

    auto functions = std::make_shared<VulkanDeviceFunctions>();
    auto get = [device, fpGetDeviceProcAddr](const char* name) {
        return fpGetDeviceProcAddr(device, name);
    };

    functions->createImage = reinterpret_cast<PFN_vkCreateImage>(get("vkCreateImage"));
    functions->destroyImage = reinterpret_cast<PFN_vkDestroyImage>(get("vkDestroyImage"));
    functions->getImageMemoryRequirements =
        reinterpret_cast<PFN_vkGetImageMemoryRequirements>(get("vkGetImageMemoryRequirements"));
    functions->allocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(get("vkAllocateMemory"));
    functions->freeMemory = reinterpret_cast<PFN_vkFreeMemory>(get("vkFreeMemory"));
    functions->bindImageMemory = reinterpret_cast<PFN_vkBindImageMemory>(get("vkBindImageMemory"));
#if defined(__APPLE__)
    functions->exportMetalObjects =
        reinterpret_cast<PFN_vkExportMetalObjectsEXT>(get("vkExportMetalObjectsEXT"));
#endif
    functions->createCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(get("vkCreateCommandPool"));
    functions->destroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(get("vkDestroyCommandPool"));
    functions->resetCommandPool = reinterpret_cast<PFN_vkResetCommandPool>(get("vkResetCommandPool"));
    functions->allocateCommandBuffers =
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(get("vkAllocateCommandBuffers"));
    functions->beginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get("vkBeginCommandBuffer"));
    functions->endCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get("vkEndCommandBuffer"));
    functions->cmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(get("vkCmdPipelineBarrier"));
    functions->cmdCopyImage = reinterpret_cast<PFN_vkCmdCopyImage>(get("vkCmdCopyImage"));
    functions->queueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(get("vkQueueSubmit"));
    functions->createFence = reinterpret_cast<PFN_vkCreateFence>(get("vkCreateFence"));
    functions->destroyFence = reinterpret_cast<PFN_vkDestroyFence>(get("vkDestroyFence"));
    functions->getFenceStatus = reinterpret_cast<PFN_vkGetFenceStatus>(get("vkGetFenceStatus"));
    functions->resetFences = reinterpret_cast<PFN_vkResetFences>(get("vkResetFences"));
    functions->waitForFences = reinterpret_cast<PFN_vkWaitForFences>(get("vkWaitForFences"));
    functions->getDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(get("vkGetDeviceQueue"));
    return functions;
}

bool RequiredVulkanSwapchainFunctionsAvailable(const VulkanDeviceFunctions& functions)
{
    return functions.createImage && functions.destroyImage &&
           functions.getImageMemoryRequirements && functions.allocateMemory &&
           functions.freeMemory && functions.bindImageMemory;
}

bool RequiredVulkanSnapshotFunctionsAvailable(const VulkanDeviceFunctions& functions)
{
    return functions.createImage && functions.destroyImage &&
           functions.getImageMemoryRequirements && functions.allocateMemory &&
           functions.freeMemory && functions.bindImageMemory &&
#if defined(__APPLE__)
           functions.exportMetalObjects &&
#endif
           functions.createCommandPool && functions.destroyCommandPool &&
           functions.resetCommandPool &&
           functions.allocateCommandBuffers && functions.beginCommandBuffer &&
           functions.endCommandBuffer && functions.cmdPipelineBarrier &&
           functions.cmdCopyImage && functions.queueSubmit &&
           functions.createFence && functions.destroyFence &&
           functions.getFenceStatus && functions.resetFences &&
           functions.waitForFences;
}

bool IsVulkanDepthFormat(int64_t format)
{
    VkFormat vkFormat = static_cast<VkFormat>(format);
    return vkFormat == VK_FORMAT_D16_UNORM ||
           vkFormat == VK_FORMAT_D32_SFLOAT ||
           vkFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
           vkFormat == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

bool IsVulkanSnapshotFormatSupported(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_UNORM ||
           format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_B8G8R8A8_UNORM ||
           format == VK_FORMAT_B8G8R8A8_SRGB;
}

uint32_t FindMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter,
                        VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps = {};
    if (gVulkanDispatch.getPhysicalDeviceMemoryProperties == nullptr)
    {
        return std::numeric_limits<uint32_t>::max();
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

    return std::numeric_limits<uint32_t>::max();
}

} // namespace

void Swapchain::InitVulkan(void* /*metalDevice*/, const VulkanGraphicsContext& vulkanContext,
                           const XrSwapchainCreateInfo* createInfo)
{
    if (createInfo == nullptr)
    {
        initializationResult_ = XR_ERROR_VALIDATION_FAILURE;
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
    vulkanContext_ = vulkanContext;
    graphicsApi_ = GraphicsApi::Vulkan;

    if (!oxrsys::swapchain::IsSupportedFormat(GraphicsApi::Vulkan, format_))
    {
        initializationResult_ = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    VkDevice device = reinterpret_cast<VkDevice>(vulkanContext.device);
    VkPhysicalDevice physDevice = reinterpret_cast<VkPhysicalDevice>(vulkanContext.physicalDevice);
    if (device == VK_NULL_HANDLE || physDevice == VK_NULL_HANDLE)
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    vulkanDeviceFunctions_ = LoadDeviceFunctions(device);
    if (vulkanDeviceFunctions_ == nullptr ||
        !RequiredVulkanSwapchainFunctionsAvailable(*vulkanDeviceFunctions_))
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: Missing Vulkan device functions for swapchain creation");
        return;
    }
    const VulkanDeviceFunctions& functions = *vulkanDeviceFunctions_;

    const bool isDepth = IsVulkanDepthFormat(format_);
#if defined(__APPLE__)
    const bool hasExportMetalObjects = functions.exportMetalObjects != nullptr;
    if (!isDepth && !hasExportMetalObjects)
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error(
            "OXRSys: Vulkan color swapchains require VK_EXT_metal_objects on macOS");
        return;
    }
#endif

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

#if defined(__APPLE__)
    VkExportMetalObjectCreateInfoEXT exportCI{};
    if (hasExportMetalObjects)
    {
        exportCI.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT;
        exportCI.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT;
        imageCI.pNext = &exportCI;
    }
#endif

    vkImages_.resize(imageCount_);
    vkMemories_.resize(imageCount_);
    textures_.resize(imageCount_, nullptr);
    imageStates_.assign(imageCount_, ImageState::Available);
    lastVulkanSnapshots_.assign(arraySize_, {});

    for (uint32_t i = 0; i < imageCount_; i++)
    {
        VkImage image = VK_NULL_HANDLE;
        VkResult result = functions.createImage(device, &imageCI, nullptr, &image);
        if (result != VK_SUCCESS)
        {
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            spdlog::error("OXRSys: vkCreateImage failed with {}", static_cast<int>(result));
            continue;
        }
        vkImages_[i] = reinterpret_cast<uint64_t>(image);

        VkMemoryRequirements memReqs{};
        functions.getImageMemoryRequirements(device, image, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(
            physDevice, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == std::numeric_limits<uint32_t>::max())
        {
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            spdlog::error("OXRSys: no device-local memory type for Vulkan swapchain");
            continue;
        }

        VkDeviceMemory memory = VK_NULL_HANDLE;
        result = functions.allocateMemory(device, &allocInfo, nullptr, &memory);
        if (result != VK_SUCCESS)
        {
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            spdlog::error("OXRSys: vkAllocateMemory failed with {}", static_cast<int>(result));
            continue;
        }
        vkMemories_[i] = reinterpret_cast<uint64_t>(memory);

        result = functions.bindImageMemory(device, image, memory, 0);
        if (result != VK_SUCCESS)
        {
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            spdlog::error("OXRSys: vkBindImageMemory failed with {}", static_cast<int>(result));
        }

#if defined(__APPLE__)
        if (hasExportMetalObjects && !isDepth)
        {
            VkExportMetalTextureInfoEXT textureInfo{};
            textureInfo.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT;
            textureInfo.image = image;
            textureInfo.plane = VK_IMAGE_ASPECT_COLOR_BIT;

            VkExportMetalObjectsInfoEXT objectsInfo{};
            objectsInfo.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
            objectsInfo.pNext = &textureInfo;

            functions.exportMetalObjects(device, &objectsInfo);
            textures_[i] = OxrsysRetainMetalObjectForSwapchain(textureInfo.mtlTexture);
        }
#endif
    }

    if (!isDepth && IsVulkanSnapshotFormatSupported(static_cast<VkFormat>(format_)) &&
        !InitVulkanSnapshotPool())
    {
        // The OpenXR swapchain remains usable. Streaming stays fail-closed until
        // a complete bounded snapshot pool can be created.
        spdlog::warn("OXRSys: Vulkan streaming snapshots are unavailable for this swapchain");
    }

    Runtime::Get().RegisterHandle(handle_, this);
    spdlog::info("OXRSys: Vulkan swapchain created {}x{} format={} arraySize={} images={}",
                 width_, height_, format_, arraySize_, imageCount_);
}

bool Swapchain::InitVulkanSnapshotPool()
{
    VkDevice device = reinterpret_cast<VkDevice>(vulkanContext_.device);
    VkPhysicalDevice physicalDevice =
        reinterpret_cast<VkPhysicalDevice>(vulkanContext_.physicalDevice);
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE ||
        vulkanDeviceFunctions_ == nullptr ||
        !RequiredVulkanSnapshotFunctionsAvailable(*vulkanDeviceFunctions_))
    {
        return false;
    }
    const VulkanDeviceFunctions& functions = *vulkanDeviceFunctions_;

    auto pool = std::make_shared<VulkanSnapshotPool>(kVulkanSnapshotSlotCount);
    pool->context = vulkanContext_;
    pool->destroyCommandPool = functions.destroyCommandPool;
    pool->destroyImage = functions.destroyImage;
    pool->freeMemory = functions.freeMemory;
    pool->destroyFence = functions.destroyFence;
    pool->waitForFences = functions.waitForFences;
    vulkanSnapshotPool_ = pool;

    VkExportMetalObjectCreateInfoEXT exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT;
    exportInfo.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT;

    for (auto& slot : pool->slots)
    {
        VkImageCreateInfo snapshotInfo{};
        snapshotInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        snapshotInfo.pNext = &exportInfo;
        snapshotInfo.imageType = VK_IMAGE_TYPE_2D;
        snapshotInfo.format = static_cast<VkFormat>(format_);
        snapshotInfo.extent = {width_, height_, 1};
        snapshotInfo.mipLevels = 1;
        snapshotInfo.arrayLayers = arraySize_;
        snapshotInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        snapshotInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        snapshotInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        snapshotInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        snapshotInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (functions.createImage(device, &snapshotInfo, nullptr,
                                  &slot.snapshotImage) != VK_SUCCESS)
        {
            DestroyVulkanSnapshotPoolResources();
            vulkanSnapshotPool_.reset();
            return false;
        }

        VkMemoryRequirements imageRequirements{};
        functions.getImageMemoryRequirements(
            device, slot.snapshotImage, &imageRequirements);
        const uint32_t memoryType = FindMemoryType(
            physicalDevice, imageRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == std::numeric_limits<uint32_t>::max())
        {
            DestroyVulkanSnapshotPoolResources();
            vulkanSnapshotPool_.reset();
            return false;
        }

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = imageRequirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (functions.allocateMemory(
                device, &allocInfo, nullptr, &slot.snapshotMemory) != VK_SUCCESS ||
            functions.bindImageMemory(
                device, slot.snapshotImage, slot.snapshotMemory, 0) != VK_SUCCESS)
        {
            DestroyVulkanSnapshotPoolResources();
            vulkanSnapshotPool_.reset();
            return false;
        }

        VkExportMetalTextureInfoEXT textureInfo{};
        textureInfo.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT;
        textureInfo.image = slot.snapshotImage;
        textureInfo.plane = VK_IMAGE_ASPECT_COLOR_BIT;
        VkExportMetalObjectsInfoEXT objectsInfo{};
        objectsInfo.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
        objectsInfo.pNext = &textureInfo;
        functions.exportMetalObjects(device, &objectsInfo);
        slot.metalTexture = OxrsysRetainMetalObjectForSwapchain(textureInfo.mtlTexture);
        if (slot.metalTexture == nullptr)
        {
            DestroyVulkanSnapshotPoolResources();
            vulkanSnapshotPool_.reset();
            return false;
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = vulkanContext_.queueFamilyIndex;
        if (functions.createCommandPool(
                device, &poolInfo, nullptr, &slot.commandPool) != VK_SUCCESS)
        {
            DestroyVulkanSnapshotPoolResources();
            vulkanSnapshotPool_.reset();
            return false;
        }

        VkCommandBufferAllocateInfo commandInfo{};
        commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandInfo.commandPool = slot.commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        if (functions.allocateCommandBuffers(
                device, &commandInfo, &slot.commandBuffer) != VK_SUCCESS)
        {
            DestroyVulkanSnapshotPoolResources();
            vulkanSnapshotPool_.reset();
            return false;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (functions.createFence(device, &fenceInfo, nullptr, &slot.fence) != VK_SUCCESS)
        {
            DestroyVulkanSnapshotPoolResources();
            vulkanSnapshotPool_.reset();
            return false;
        }
    }

    spdlog::info("OXRSys: Vulkan snapshot pool preallocated slots={}",
                 pool->slots.size());
    return true;
}

XrResult Swapchain::EnumerateVulkanImages(uint32_t /*imageCapacityInput*/,
                                          XrSwapchainImageBaseHeader* images) const
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

bool Swapchain::DestroyVulkanResources()
{
    if (vkDevice_ == nullptr)
    {
        return true;
    }

    // Drop the swapchain-owned references first. Encoder work already accepted
    // by the latest-frame queue retains its own slot lease.
    lastVulkanSnapshots_.clear();

    if (vulkanSnapshotPool_ != nullptr)
    {
        if (!DrainVulkanSnapshotPool(kVulkanDestroyDrainTimeout))
        {
            // Do not return success to xrDestroySwapchain/session: the app may
            // destroy VkDevice immediately afterwards. Preserve every handle
            // and make the public destruction call retryable instead.
            spdlog::warn(
                "OXRSys: Vulkan snapshot drain timed out; destruction must be retried");
            return false;
        }

        DestroyVulkanSnapshotPoolResources();
        vulkanSnapshotPool_.reset();
    }

    VkDevice device = reinterpret_cast<VkDevice>(vkDevice_);
    const auto functions = vulkanDeviceFunctions_;
    for (uint32_t i = 0; i < imageCount_; i++)
    {
#if defined(__APPLE__) && defined(XR_USE_GRAPHICS_API_METAL)
        if (i < textures_.size() && textures_[i] != nullptr)
        {
            ReleaseMetalTextureSlice(textures_[i]);
            textures_[i] = nullptr;
        }
#endif
        if (i < vkImages_.size() && vkImages_[i] != 0 &&
            functions != nullptr && functions->destroyImage != nullptr)
        {
            functions->destroyImage(device, reinterpret_cast<VkImage>(vkImages_[i]), nullptr);
        }
        if (i < vkMemories_.size() && vkMemories_[i] != 0 &&
            functions != nullptr && functions->freeMemory != nullptr)
        {
            functions->freeMemory(
                device, reinterpret_cast<VkDeviceMemory>(vkMemories_[i]), nullptr);
        }
    }
    textures_.clear();
    vkImages_.clear();
    vkMemories_.clear();
    vkDevice_ = nullptr;
    vulkanDeviceFunctions_.reset();
    return true;
}

bool Swapchain::DrainVulkanSnapshotPool(std::chrono::nanoseconds timeout)
{
    const auto pool = vulkanSnapshotPool_;
    if (pool == nullptr)
    {
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    pool->stopping.store(true, std::memory_order_release);
    if (!pool->leases.StopAndWaitForLeases(timeout))
    {
        return false;
    }

    std::vector<VkFence> submittedFences;
    submittedFences.reserve(pool->slots.size());
    for (const auto& slot : pool->slots)
    {
        if (slot.fenceSubmitted && slot.fence != VK_NULL_HANDLE)
        {
            submittedFences.push_back(slot.fence);
        }
    }
    if (submittedFences.empty())
    {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline || pool->waitForFences == nullptr)
    {
        return false;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
    const uint64_t remainingNs = static_cast<uint64_t>(remaining.count());
    const VkDevice device = reinterpret_cast<VkDevice>(pool->context.device);
    return device != VK_NULL_HANDLE &&
           pool->waitForFences(device, static_cast<uint32_t>(submittedFences.size()),
                               submittedFences.data(), VK_TRUE, remainingNs) == VK_SUCCESS;
}

void Swapchain::DestroyVulkanSnapshotPoolResources()
{
    const auto pool = vulkanSnapshotPool_;
    if (pool == nullptr)
    {
        return;
    }

    const VkDevice device = reinterpret_cast<VkDevice>(pool->context.device);
    for (auto& slot : pool->slots)
    {
        if (slot.metalTexture != nullptr)
        {
            ReleaseTextureSlice(slot.metalTexture);
            slot.metalTexture = nullptr;
        }
        if (device != VK_NULL_HANDLE && slot.fence != VK_NULL_HANDLE &&
            pool->destroyFence != nullptr)
        {
            pool->destroyFence(device, slot.fence, nullptr);
            slot.fence = VK_NULL_HANDLE;
        }
        if (device != VK_NULL_HANDLE && slot.commandPool != VK_NULL_HANDLE &&
            pool->destroyCommandPool != nullptr)
        {
            pool->destroyCommandPool(device, slot.commandPool, nullptr);
            slot.commandPool = VK_NULL_HANDLE;
            slot.commandBuffer = VK_NULL_HANDLE;
        }
        if (device != VK_NULL_HANDLE && slot.snapshotImage != VK_NULL_HANDLE &&
            pool->destroyImage != nullptr)
        {
            pool->destroyImage(device, slot.snapshotImage, nullptr);
            slot.snapshotImage = VK_NULL_HANDLE;
        }
        if (device != VK_NULL_HANDLE && slot.snapshotMemory != VK_NULL_HANDLE &&
            pool->freeMemory != nullptr)
        {
            pool->freeMemory(device, slot.snapshotMemory, nullptr);
            slot.snapshotMemory = VK_NULL_HANDLE;
        }
        slot.fenceSubmitted = false;
    }
}

void Swapchain::SnapshotVulkanReleasedImage(bool requested)
{
    lastVulkanSnapshots_.assign(arraySize_, {});
    const auto pool = vulkanSnapshotPool_;
    if (!requested || pool == nullptr || vkImages_.empty() ||
        pool->stopping.load(std::memory_order_acquire) ||
        !IsVulkanSnapshotFormatSupported(static_cast<VkFormat>(format_)))
    {
        return;
    }

    VkDevice device = reinterpret_cast<VkDevice>(vulkanContext_.device);
    VkImage image = reinterpret_cast<VkImage>(vkImages_[lastReleasedIndex_]);
    if (device == VK_NULL_HANDLE || image == VK_NULL_HANDLE)
    {
        return;
    }

    const auto functions = vulkanDeviceFunctions_;
    if (functions == nullptr ||
        !RequiredVulkanSnapshotFunctionsAvailable(*functions))
    {
        spdlog::warn("OXRSys: Vulkan-to-Metal snapshot functions are unavailable");
        return;
    }

    VkQueue queue = reinterpret_cast<VkQueue>(vulkanContext_.queue);
    if (queue == VK_NULL_HANDLE && functions->getDeviceQueue != nullptr)
    {
        functions->getDeviceQueue(
            device, vulkanContext_.queueFamilyIndex, vulkanContext_.queueIndex, &queue);
    }
    if (queue == VK_NULL_HANDLE)
    {
        return;
    }

    const SnapshotLeasePool::Lease lease = pool->leases.TryAcquire([&](size_t index) {
        const auto& candidate = pool->slots[index];
        return !candidate.fenceSubmitted ||
               functions->getFenceStatus(device, candidate.fence) == VK_SUCCESS;
    });
    if (!lease)
    {
        spdlog::debug("OXRSys: Vulkan snapshot pool is full; streaming snapshot unavailable");
        return;
    }

    VulkanSnapshotSlot& slot = pool->slots[lease.index];
    if (functions->resetFences(device, 1, &slot.fence) != VK_SUCCESS ||
        functions->resetCommandPool(device, slot.commandPool, 0) != VK_SUCCESS)
    {
        slot.fenceSubmitted = false;
        return;
    }
    slot.fenceSubmitted = false;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (functions->beginCommandBuffer(slot.commandBuffer, &beginInfo) != VK_SUCCESS)
    {
        return;
    }

    VkImageMemoryBarrier toTransfer[2]{};
    toTransfer[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer[0].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toTransfer[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toTransfer[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer[0].image = image;
    toTransfer[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer[0].subresourceRange.baseMipLevel = 0;
    toTransfer[0].subresourceRange.levelCount = 1;
    toTransfer[0].subresourceRange.baseArrayLayer = 0;
    toTransfer[0].subresourceRange.layerCount = arraySize_;

    toTransfer[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer[1].srcAccessMask = 0;
    toTransfer[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer[1].image = slot.snapshotImage;
    toTransfer[1].subresourceRange = toTransfer[0].subresourceRange;
    functions->cmdPipelineBarrier(
        slot.commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, toTransfer);

    VkImageCopy region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = 0;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = arraySize_;
    region.srcOffset = {0, 0, 0};
    region.dstSubresource = region.srcSubresource;
    region.dstOffset = {0, 0, 0};
    region.extent = {width_, height_, 1};
    functions->cmdCopyImage(
        slot.commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        slot.snapshotImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier afterTransfer[2]{};
    afterTransfer[0] = toTransfer[0];
    afterTransfer[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    afterTransfer[0].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    afterTransfer[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    afterTransfer[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    afterTransfer[1] = toTransfer[1];
    afterTransfer[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    afterTransfer[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    afterTransfer[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    afterTransfer[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    functions->cmdPipelineBarrier(
        slot.commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 2, afterTransfer);

    if (functions->endCommandBuffer(slot.commandBuffer) != VK_SUCCESS)
    {
        return;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &slot.commandBuffer;
    if (functions->queueSubmit(queue, 1, &submitInfo, slot.fence) != VK_SUCCESS)
    {
        return;
    }
    slot.fenceSubmitted = true;
    const std::shared_ptr<void> snapshotLifetime = lease.lifetime;

    for (uint32_t arrayIndex = 0; arrayIndex < arraySize_; ++arrayIndex)
    {
        void* textureSlice = OxrsysCreateMetalTextureSliceForSwapchain(
            slot.metalTexture, arraySize_, arrayIndex);
        if (textureSlice == nullptr)
        {
            continue;
        }

        FrameImageSource frameSource = {};
        frameSource.api = GraphicsApi::Vulkan;
        frameSource.image = std::shared_ptr<void>(textureSlice, [snapshotLifetime](void* texture) {
            Swapchain::ReleaseTextureSlice(texture);
        });
        frameSource.lifetime = snapshotLifetime;
        frameSource.sync.kind = FrameSyncKind::HostFence;
        frameSource.sync.waitObject = snapshotLifetime;
        frameSource.sync.waitForReady = [pool, slotIndex = lease.index](uint64_t timeoutNs) {
            // Stop-before-drain prevents a queued encoder frame from starting a
            // new Vulkan call once swapchain teardown begins.
            if (pool->stopping.load(std::memory_order_acquire) ||
                slotIndex >= pool->slots.size())
            {
                return false;
            }
            const auto& snapshot = pool->slots[slotIndex];
            VkDevice snapshotDevice = reinterpret_cast<VkDevice>(pool->context.device);
            return snapshotDevice != VK_NULL_HANDLE && snapshot.fenceSubmitted &&
                   snapshot.fence != VK_NULL_HANDLE && pool->waitForFences != nullptr &&
                   pool->waitForFences(snapshotDevice, 1, &snapshot.fence,
                                       VK_TRUE, timeoutNs) == VK_SUCCESS;
        };
        frameSource.sourceWidth = width_;
        frameSource.sourceHeight = height_;
        frameSource.sourceFormat = static_cast<uint64_t>(format_);
        frameSource.imageWidth = width_;
        frameSource.imageHeight = height_;
        lastVulkanSnapshots_[arrayIndex] = std::move(frameSource);
    }
}
