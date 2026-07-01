// SPDX-License-Identifier: MPL-2.0

#include "Swapchain.h"
#include "Runtime.h"
#include "VulkanDispatch.h"

#include <spdlog/spdlog.h>
#include <vulkan/vulkan.h>
#if defined(__APPLE__)
#include <vulkan/vulkan_metal.h>
#endif

#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

#if defined(__APPLE__)
extern "C" void* OxrsysRetainMetalObjectForSwapchain(void* object);
#endif

namespace
{

struct VkDeviceFuncs
{
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
#if defined(__APPLE__)
    PFN_vkExportMetalObjectsEXT exportMetalObjects = nullptr;
#endif
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
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkInvalidateMappedMemoryRanges invalidateMappedMemoryRanges = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
};

VkDeviceFuncs gDeviceFuncs;
VkDevice gLastLoadedDevice = VK_NULL_HANDLE;
constexpr uint32_t kBackendSnapshotCount = Swapchain::SwapchainImageCount * 2 + 2;

std::shared_ptr<void> TryAcquireBackendSnapshotLease(
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

void DestroyVulkanFrameSourceBlocking(Swapchain::VulkanFrameSource* source)
{
    if (source == nullptr)
    {
        return;
    }

    VkDevice device = reinterpret_cast<VkDevice>(source->context.device);
    if (source->fenceSubmitted && source->fence != VK_NULL_HANDLE && source->waitForFences != nullptr)
    {
        source->waitForFences(device, 1, &source->fence, VK_TRUE, UINT64_MAX);
    }
    if (source->stagingBuffer != VK_NULL_HANDLE && source->destroyBuffer != nullptr)
    {
        source->destroyBuffer(device, source->stagingBuffer, nullptr);
    }
    if (source->stagingMemory != VK_NULL_HANDLE && source->freeMemory != nullptr)
    {
        source->freeMemory(device, source->stagingMemory, nullptr);
    }
    if (source->fence != VK_NULL_HANDLE && source->destroyFence != nullptr)
    {
        source->destroyFence(device, source->fence, nullptr);
    }
    if (source->commandPool != VK_NULL_HANDLE && source->destroyCommandPool != nullptr)
    {
        source->destroyCommandPool(device, source->commandPool, nullptr);
    }
    delete source;
}

class VulkanFrameSourceCleanupQueue
{
public:
    VulkanFrameSourceCleanupQueue()
        : worker_([this] { Run(); })
    {
    }

    ~VulkanFrameSourceCleanupQueue()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    void Enqueue(Swapchain::VulkanFrameSource* source)
    {
        if (source == nullptr)
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.push_back(source);
        }
        cv_.notify_one();
    }

private:
    void Run()
    {
        for (;;)
        {
            Swapchain::VulkanFrameSource* source = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return stopping_ || !pending_.empty();
                });
                if (pending_.empty())
                {
                    if (stopping_)
                    {
                        return;
                    }
                    continue;
                }
                source = pending_.front();
                pending_.pop_front();
            }
            DestroyVulkanFrameSourceBlocking(source);
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Swapchain::VulkanFrameSource*> pending_;
    std::thread worker_;
    bool stopping_ = false;
};

VulkanFrameSourceCleanupQueue& VulkanCleanupQueue()
{
    static VulkanFrameSourceCleanupQueue queue;
    return queue;
}

void LoadDeviceFunctions(VkDevice device)
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
#if defined(__APPLE__)
    gDeviceFuncs.exportMetalObjects =
        reinterpret_cast<PFN_vkExportMetalObjectsEXT>(get("vkExportMetalObjectsEXT"));
#endif
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
    gDeviceFuncs.mapMemory = reinterpret_cast<PFN_vkMapMemory>(get("vkMapMemory"));
    gDeviceFuncs.unmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(get("vkUnmapMemory"));
    gDeviceFuncs.invalidateMappedMemoryRanges =
        reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(get("vkInvalidateMappedMemoryRanges"));
    gDeviceFuncs.getDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(get("vkGetDeviceQueue"));
}

bool RequiredVulkanSwapchainFunctionsAvailable()
{
    return gDeviceFuncs.createImage && gDeviceFuncs.destroyImage &&
           gDeviceFuncs.getImageMemoryRequirements && gDeviceFuncs.allocateMemory &&
           gDeviceFuncs.freeMemory && gDeviceFuncs.bindImageMemory;
}

bool RequiredVulkanSnapshotFunctionsAvailable()
{
    return gDeviceFuncs.createBuffer && gDeviceFuncs.destroyBuffer &&
           gDeviceFuncs.getBufferMemoryRequirements && gDeviceFuncs.allocateMemory &&
           gDeviceFuncs.freeMemory && gDeviceFuncs.bindBufferMemory &&
           gDeviceFuncs.createCommandPool && gDeviceFuncs.destroyCommandPool &&
           gDeviceFuncs.allocateCommandBuffers && gDeviceFuncs.beginCommandBuffer &&
           gDeviceFuncs.endCommandBuffer && gDeviceFuncs.cmdPipelineBarrier &&
           gDeviceFuncs.cmdCopyImageToBuffer && gDeviceFuncs.queueSubmit &&
           gDeviceFuncs.createFence && gDeviceFuncs.destroyFence &&
           gDeviceFuncs.waitForFences && gDeviceFuncs.mapMemory && gDeviceFuncs.unmapMemory;
}

bool IsVulkanDepthFormat(int64_t format)
{
    VkFormat vkFormat = static_cast<VkFormat>(format);
    return vkFormat == VK_FORMAT_D16_UNORM ||
           vkFormat == VK_FORMAT_D32_SFLOAT ||
           vkFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
           vkFormat == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

bool IsVulkanReadbackFormatSupported(VkFormat format)
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

    VkDevice device = reinterpret_cast<VkDevice>(vulkanContext.device);
    VkPhysicalDevice physDevice = reinterpret_cast<VkPhysicalDevice>(vulkanContext.physicalDevice);
    if (device == VK_NULL_HANDLE || physDevice == VK_NULL_HANDLE)
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    LoadDeviceFunctions(device);
    if (!RequiredVulkanSwapchainFunctionsAvailable())
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: Missing Vulkan device functions for swapchain creation");
        return;
    }

    const bool isDepth = IsVulkanDepthFormat(format_);
#if defined(__APPLE__)
    const bool hasExportMetalObjects = gDeviceFuncs.exportMetalObjects != nullptr;
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
        VkResult result = gDeviceFuncs.createImage(device, &imageCI, nullptr, &image);
        if (result != VK_SUCCESS)
        {
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
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
        if (allocInfo.memoryTypeIndex == std::numeric_limits<uint32_t>::max())
        {
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            spdlog::error("OXRSys: no device-local memory type for Vulkan swapchain");
            continue;
        }

        VkDeviceMemory memory = VK_NULL_HANDLE;
        result = gDeviceFuncs.allocateMemory(device, &allocInfo, nullptr, &memory);
        if (result != VK_SUCCESS)
        {
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            spdlog::error("OXRSys: vkAllocateMemory failed with {}", static_cast<int>(result));
            continue;
        }
        vkMemories_[i] = reinterpret_cast<uint64_t>(memory);

        result = gDeviceFuncs.bindImageMemory(device, image, memory, 0);
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

            gDeviceFuncs.exportMetalObjects(device, &objectsInfo);
            textures_[i] = OxrsysRetainMetalObjectForSwapchain(textureInfo.mtlTexture);
        }
#endif
    }

    Runtime::Get().RegisterHandle(handle_, this);
    spdlog::info("OXRSys: Vulkan swapchain created {}x{} format={} arraySize={} images={}",
                 width_, height_, format_, arraySize_, imageCount_);
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

void Swapchain::DestroyVulkanResources()
{
    if (vkDevice_ == nullptr)
    {
        return;
    }

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
#if defined(__APPLE__) && defined(XR_USE_GRAPHICS_API_METAL)
        if (i < textures_.size() && textures_[i] != nullptr)
        {
            ReleaseMetalTextureSlice(textures_[i]);
            textures_[i] = nullptr;
        }
#endif
    }
}

FrameImageSource Swapchain::SnapshotVulkanFrameImageSource(uint32_t arrayIndex) const
{
    if (arrayIndex >= arraySize_ || vkImages_.empty() ||
        !IsVulkanReadbackFormatSupported(static_cast<VkFormat>(format_)))
    {
        return {};
    }

    std::shared_ptr<void> lease = TryAcquireBackendSnapshotLease(backendSnapshotPool_);
    if (!lease)
    {
        spdlog::debug("OXRSys: Vulkan snapshot pool is full; streaming snapshot unavailable");
        return {};
    }

    VkDevice device = reinterpret_cast<VkDevice>(vulkanContext_.device);
    VkPhysicalDevice physicalDevice = reinterpret_cast<VkPhysicalDevice>(vulkanContext_.physicalDevice);
    VkImage image = reinterpret_cast<VkImage>(vkImages_[lastReleasedIndex_]);
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || image == VK_NULL_HANDLE)
    {
        return {};
    }

    LoadDeviceFunctions(device);
    if (!RequiredVulkanSnapshotFunctionsAvailable())
    {
        return {};
    }

    VkQueue queue = reinterpret_cast<VkQueue>(vulkanContext_.queue);
    if (queue == VK_NULL_HANDLE && gDeviceFuncs.getDeviceQueue != nullptr)
    {
        gDeviceFuncs.getDeviceQueue(
            device, vulkanContext_.queueFamilyIndex, vulkanContext_.queueIndex, &queue);
    }
    if (queue == VK_NULL_HANDLE)
    {
        return {};
    }

    auto* source = new VulkanFrameSource();
    source->lifetime = lease;
    source->context = vulkanContext_;
    source->context.queue = reinterpret_cast<void*>(queue);
    source->format = static_cast<VkFormat>(format_);
    source->width = width_;
    source->height = height_;
    source->destroyCommandPool = gDeviceFuncs.destroyCommandPool;
    source->destroyBuffer = gDeviceFuncs.destroyBuffer;
    source->freeMemory = gDeviceFuncs.freeMemory;
    source->destroyFence = gDeviceFuncs.destroyFence;
    source->waitForFences = gDeviceFuncs.waitForFences;
    source->mapMemory = gDeviceFuncs.mapMemory;
    source->unmapMemory = gDeviceFuncs.unmapMemory;
    source->invalidateMappedMemoryRanges = gDeviceFuncs.invalidateMappedMemoryRanges;

    const VkDeviceSize readbackSize = static_cast<VkDeviceSize>(width_) * height_ * 4u;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = readbackSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (gDeviceFuncs.createBuffer(device, &bufferInfo, nullptr, &source->stagingBuffer) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkMemoryRequirements bufferReqs{};
    gDeviceFuncs.getBufferMemoryRequirements(device, source->stagingBuffer, &bufferReqs);
    uint32_t memoryType = FindMemoryType(
        physicalDevice, bufferReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == std::numeric_limits<uint32_t>::max())
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = bufferReqs.size;
    allocInfo.memoryTypeIndex = memoryType;
    if (gDeviceFuncs.allocateMemory(device, &allocInfo, nullptr, &source->stagingMemory) != VK_SUCCESS ||
        gDeviceFuncs.bindBufferMemory(device, source->stagingBuffer, source->stagingMemory, 0) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }
    source->stagingSize = static_cast<size_t>(readbackSize);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = vulkanContext_.queueFamilyIndex;
    if (gDeviceFuncs.createCommandPool(device, &poolInfo, nullptr, &source->commandPool) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = source->commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    if (gDeviceFuncs.allocateCommandBuffers(device, &commandInfo, &commandBuffer) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (gDeviceFuncs.createFence(device, &fenceInfo, nullptr, &source->fence) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (gDeviceFuncs.beginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }

    VkImageMemoryBarrier toTransfer{};
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

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = width_;
    region.bufferImageHeight = height_;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = arrayIndex;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width_, height_, 1};
    gDeviceFuncs.cmdCopyImageToBuffer(
        commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        source->stagingBuffer, 1, &region);

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

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    if (gDeviceFuncs.queueSubmit(queue, 1, &submitInfo, source->fence) != VK_SUCCESS)
    {
        ReleaseVulkanFrameSource(source);
        return {};
    }
    source->fenceSubmitted = true;

    FrameImageSource frameSource = {};
    frameSource.api = GraphicsApi::Vulkan;
    frameSource.image = std::shared_ptr<void>(source, [](void* ptr) {
        Swapchain::ReleaseVulkanFrameSource(static_cast<VulkanFrameSource*>(ptr));
    });
    frameSource.lifetime = lease;
    frameSource.sourceWidth = width_;
    frameSource.sourceHeight = height_;
    frameSource.sourceFormat = static_cast<uint64_t>(source->format);
    frameSource.imageWidth = width_;
    frameSource.imageHeight = height_;
    return frameSource;
}

void Swapchain::ReleaseVulkanFrameSource(VulkanFrameSource* source)
{
    if (source == nullptr)
    {
        return;
    }

    if (source->fenceSubmitted && source->fence != VK_NULL_HANDLE && source->waitForFences != nullptr)
    {
        VulkanCleanupQueue().Enqueue(source);
        return;
    }
    DestroyVulkanFrameSourceBlocking(source);
}
