// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>
#include "GraphicsTypes.h"
#ifdef XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#endif
#include <openxr/openxr_platform.h>
#include <vector>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>

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

    // Acquire a backend-native image source for streaming. Dynamic Metal swapchains
    // prefer a release-time staging snapshot; Vulkan currently returns the live
    // image handle as the Linux readback path is still scaffolded.
    FrameImageSource GetLastReleasedFrameImageSource(uint32_t arrayIndex) const;

    uint32_t GetArraySize() const
    {
        return arraySize_;
    }

    bool HasReleasedImage() const;

    static constexpr uint32_t SwapchainImageCount = 3;

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

    uint64_t handle_ = 0;
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

    // Vulkan resources (only used when graphicsApi_ == Vulkan)
    void* vkDevice_ = nullptr;
    std::vector<uint64_t> vkImages_;   // VkImage handles
    std::vector<uint64_t> vkMemories_; // VkDeviceMemory handles

    uint32_t nextAcquireIndex_ = 0;
    uint32_t lastReleasedIndex_ = 0;
    bool staticImageAcquired_ = false;
    bool hasReleasedImage_ = false;
    std::vector<ImageState> imageStates_;
    std::deque<uint32_t> acquiredImageOrder_;
    mutable std::mutex stateMutex_;
};
