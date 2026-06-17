// SPDX-License-Identifier: MPL-2.0

#pragma once

#ifdef XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#endif
#if defined(_WIN32)
#include "D3DGraphicsContext.h"
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "GraphicsTypes.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

struct SwapchainStagingSlotState
{
    std::atomic_bool inUse{false};
};

class Swapchain
{
public:
    Swapchain(void* metalDevice, const XrSwapchainCreateInfo* createInfo);
    Swapchain(const GraphicsContext& graphicsContext, const XrSwapchainCreateInfo* createInfo);

#ifdef XR_USE_GRAPHICS_API_VULKAN
    Swapchain(GraphicsApi api, void* metalDevice,
              const VulkanGraphicsContext* vulkanContext,
              const XrSwapchainCreateInfo* createInfo);
#endif
#if defined(_WIN32)
    Swapchain(GraphicsApi api, const D3D11GraphicsContext* d3d11Context,
              const XrSwapchainCreateInfo* createInfo);
    Swapchain(GraphicsApi api, const D3D12GraphicsContext* d3d12Context,
              const XrSwapchainCreateInfo* createInfo);
#endif

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

    uint32_t GetWidth() const { return width_; }
    uint32_t GetHeight() const { return height_; }
    int64_t GetFormat() const { return format_; }
    uint32_t GetImageCount() const { return imageCount_; }

    void* GetLastReleasedTexture() const;
    void* GetLastReleasedTextureSlice(uint32_t arrayIndex) const;
    static void ReleaseTextureSlice(void* textureSlice);

    FrameImageSource GetLastReleasedFrameImageSource(uint32_t arrayIndex) const;

    uint32_t GetArraySize() const
    {
        return arraySize_;
    }

    bool HasReleasedImage() const;

    static constexpr uint32_t SwapchainImageCount = 3;

#ifdef XR_USE_GRAPHICS_API_VULKAN
    struct VulkanFrameSource
    {
        Swapchain* owner = nullptr;
        const VulkanGraphicsContext* context = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t arrayLayer = 0;
        uint32_t imageIndex = 0;
    };
#endif

#if defined(_WIN32)
    struct D3D11FrameSource
    {
        Swapchain* owner = nullptr;
        const D3D11GraphicsContext* context = nullptr;
        ID3D11Texture2D* texture = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t arrayLayer = 0;
        uint32_t imageIndex = 0;
    };

    struct D3D12FrameSource
    {
        Swapchain* owner = nullptr;
        const D3D12GraphicsContext* context = nullptr;
        ID3D12Resource* texture = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t arrayLayer = 0;
        uint32_t imageIndex = 0;
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
#ifdef XR_USE_GRAPHICS_API_VULKAN
    void InitVulkan(void* metalDevice, const VulkanGraphicsContext* vulkanContext,
                    const XrSwapchainCreateInfo* createInfo);
#endif
#if defined(_WIN32)
    void InitD3D11(const D3D11GraphicsContext* d3d11Context,
                   const XrSwapchainCreateInfo* createInfo);
    void InitD3D12(const D3D12GraphicsContext* d3d12Context,
                   const XrSwapchainCreateInfo* createInfo);
#endif
    void InitMetalStaging(void* metalDevice);
    void RetainImage(uint32_t imageIndex);
    void ReleaseImageRetention(uint32_t imageIndex);

#ifdef XR_USE_GRAPHICS_API_VULKAN
    static bool TryReleaseVulkanFrameSource(void* textureSlice);
#endif
#if defined(_WIN32)
    static bool TryReleaseD3D11FrameSource(void* textureSlice);
    static bool TryReleaseD3D12FrameSource(void* textureSlice);
#endif

    uint64_t handle_ = 0;
    GraphicsApi graphicsApi_ = GraphicsApi::Metal;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int64_t format_ = 0;
    uint32_t arraySize_ = 1;
    uint32_t imageCount_ = SwapchainImageCount;

    void* device_ = nullptr; // MTL::Device*
    void* metalCommandQueue_ = nullptr; // id<MTLCommandQueue>, app-owned
    std::vector<void*> textures_;
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

#ifdef XR_USE_GRAPHICS_API_VULKAN
    const VulkanGraphicsContext* vulkanContext_ = nullptr;
#endif
    std::vector<uint64_t> vkImages_;
    std::vector<uint64_t> vkMemories_;

#if defined(_WIN32)
    const D3D11GraphicsContext* d3d11Context_ = nullptr;
    const D3D12GraphicsContext* d3d12Context_ = nullptr;
    std::vector<void*> d3d11Textures_;
    std::vector<void*> d3d12Resources_;
#endif

    std::vector<uint32_t> imageRetainCounts_;

    uint32_t nextAcquireIndex_ = 0;
    uint32_t lastReleasedIndex_ = 0;
    bool staticImageAcquired_ = false;
    bool hasReleasedImage_ = false;
    std::vector<ImageState> imageStates_;
    std::deque<uint32_t> acquiredImageOrder_;
    std::deque<bool> acquiredImageWaitEligible_;
    mutable std::mutex stateMutex_;
};
