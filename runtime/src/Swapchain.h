// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>
#include "GraphicsTypes.h"
#ifdef XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#if defined(__APPLE__)
#include <vulkan/vulkan_metal.h>
#endif

struct VulkanDeviceFunctions
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
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkResetCommandPool resetCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkGetFenceStatus getFenceStatus = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
};
#endif
#include <vector>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstddef>

#include "SnapshotLeasePool.h"

struct SwapchainStagingSlotState
{
    std::atomic_bool inUse{false};
};

class Swapchain
{
public:
    Swapchain(const GraphicsContext& graphicsContext, const XrSwapchainCreateInfo* createInfo);

    ~Swapchain();

    uint64_t GetHandle() const
    {
        return handle_;
    }

    GraphicsApi GetGraphicsApi() const
    {
        return graphicsApi_;
    }

    XrResult EnumerateImages(uint32_t imageCapacityInput, uint32_t* imageCountOutput,
                              XrSwapchainImageBaseHeader* images);
    XrResult AcquireImage(const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index);
    XrResult WaitImage(const XrSwapchainImageWaitInfo* waitInfo);
    XrResult ReleaseImage(const XrSwapchainImageReleaseInfo* releaseInfo);
    XrResult InitializationResult() const { return initializationResult_; }
    // Retryable pre-destruction barrier. Failure keeps the OpenXR handle and
    // every backend resource alive for a later xrDestroy* retry.
    XrResult PrepareForDestroy();

    uint32_t GetWidth() const
    {
        return width_;
    }
    uint32_t GetHeight() const
    {
        return height_;
    }
    int64_t GetFormat() const
    {
        return format_;
    }
    uint32_t GetImageCount() const
    {
        return imageCount_;
    }

    // Get the most recently released texture (MTLTexture* for debug rendering)
    void* GetLastReleasedTexture() const;

    // Get a texture view for a specific array slice of the last released texture.
    // For non-array textures (arraySize==1), returns the texture as-is.
    // Caller must call ReleaseTextureSlice() on the returned pointer when done.
    void* GetLastReleasedTextureSlice(uint32_t arrayIndex) const;

    // Release a texture view obtained from GetLastReleasedTextureSlice.
    static void ReleaseTextureSlice(void* textureSlice);

    // Acquire a backend-native image source for streaming. Dynamic backends prefer
    // release-time snapshots so encoding can wait/read outside xrEndFrame.
    FrameImageSource GetLastReleasedFrameImageSource(uint32_t arrayIndex) const;

    uint32_t GetArraySize() const
    {
        return arraySize_;
    }

    bool HasReleasedImage() const;

    // Release-time snapshots are only useful while a headset encoder is ready.
    // The shared flag avoids coupling swapchains to StreamingServer lifetime.
    void SetStreamingSnapshotDemand(const std::shared_ptr<std::atomic_bool>& demand)
    {
        streamingSnapshotDemand_ = demand;
    }

    static constexpr uint32_t SwapchainImageCount = 3;

#ifdef XR_USE_GRAPHICS_API_VULKAN
    struct VulkanSnapshotSlot
    {
        VkImage snapshotImage = VK_NULL_HANDLE;
        VkDeviceMemory snapshotMemory = VK_NULL_HANDLE;
        void* metalTexture = nullptr;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool fenceSubmitted = false;
    };

    struct VulkanSnapshotPool
    {
        explicit VulkanSnapshotPool(size_t slotCount)
            : leases(slotCount), slots(slotCount)
        {
        }

        SnapshotLeasePool leases;
        std::vector<VulkanSnapshotSlot> slots;
        VulkanGraphicsContext context = {};
        PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
        PFN_vkDestroyImage destroyImage = nullptr;
        PFN_vkFreeMemory freeMemory = nullptr;
        PFN_vkDestroyFence destroyFence = nullptr;
        PFN_vkWaitForFences waitForFences = nullptr;
        // Once stopping is visible, no new encoder-side Vulkan wait may start.
        // Existing waits retain a lease and are covered by the bounded drain.
        std::atomic_bool stopping{false};
    };
#endif

private:
    enum class ImageState
    {
        Available,
        Acquired,
        Waited,
    };

    void InitMetal(void* metalDevice, const XrSwapchainCreateInfo* createInfo);
    void InitVulkan(void* metalDevice, const VulkanGraphicsContext& vulkanContext,
                     const XrSwapchainCreateInfo* createInfo);
    void InitMetalStaging(void* metalDevice);
#ifdef XR_USE_GRAPHICS_API_METAL
    XrResult EnumerateMetalImages(uint32_t imageCapacityInput,
                                  XrSwapchainImageBaseHeader* images) const;
    void DestroyMetalResources();
    void SnapshotMetalReleasedImage(bool requested);
    void* GetLastReleasedMetalTextureSlice(uint32_t arrayIndex) const;
    FrameImageSource SnapshotMetalFrameImageSource(uint32_t arrayIndex) const;
    static void ReleaseMetalTextureSlice(void* textureSlice);
#endif
#ifdef XR_USE_GRAPHICS_API_VULKAN
    XrResult EnumerateVulkanImages(uint32_t imageCapacityInput,
                                   XrSwapchainImageBaseHeader* images) const;
    bool DestroyVulkanResources();
    void SnapshotVulkanReleasedImage(bool requested);
    bool InitVulkanSnapshotPool();
    bool DrainVulkanSnapshotPool(std::chrono::nanoseconds timeout);
    void DestroyVulkanSnapshotPoolResources();
#endif
    uint64_t handle_ = 0;
    XrResult initializationResult_ = XR_SUCCESS;
    GraphicsApi graphicsApi_ = GraphicsApi::Metal;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int64_t format_ = 0;
    uint32_t arraySize_ = 1;
    uint32_t imageCount_ = SwapchainImageCount;

    void* device_ = nullptr; // MTL::Device*
    void* metalCommandQueue_ = nullptr; // id<MTLCommandQueue>, app-owned
    std::vector<void*> textures_; // MTL::Texture* (always Metal textures, for debug rendering)
    void* snapshotEvent_ = nullptr; // id<MTLSharedEvent>

    struct StagingSlot
    {
        void* texture = nullptr; // id<MTLTexture>
        std::shared_ptr<SwapchainStagingSlotState> state = {};
    };

    std::vector<StagingSlot> stagingSlots_;
    uint32_t nextStagingIndex_ = 0;
    uint32_t lastSnapshotIndex_ = 0;
    uint64_t lastSnapshotValue_ = 0;
    bool hasSnapshot_ = false;
    std::shared_ptr<void> lastSnapshotLease_ = {};
    std::shared_ptr<std::atomic_bool> streamingSnapshotDemand_ = {};

    // Vulkan resources (only used when graphicsApi_ == Vulkan)
    void* vkDevice_ = nullptr;
    VulkanGraphicsContext vulkanContext_ = {};
    std::shared_ptr<const VulkanDeviceFunctions> vulkanDeviceFunctions_ = {};
    std::vector<uint64_t> vkImages_;   // VkImage handles
    std::vector<uint64_t> vkMemories_; // VkDeviceMemory handles
    std::vector<FrameImageSource> lastVulkanSnapshots_;
    std::shared_ptr<VulkanSnapshotPool> vulkanSnapshotPool_ = {};
    bool resourcesDestroyed_ = false;
    std::mutex teardownMutex_;

    uint32_t nextAcquireIndex_ = 0;
    uint32_t lastReleasedIndex_ = 0;
    bool staticImageAcquired_ = false;
    bool hasReleasedImage_ = false;
    std::vector<ImageState> imageStates_;
    std::deque<uint32_t> acquiredImageOrder_;
    mutable std::mutex stateMutex_;
};
