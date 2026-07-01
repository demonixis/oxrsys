// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>
#include "GraphicsTypes.h"
#ifdef XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#endif
#ifdef XR_USE_GRAPHICS_API_OPENGL
#include <GL/gl.h>
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(OXRSYS_USE_D3D12) || \
                        defined(XR_USE_GRAPHICS_API_D3D11) || defined(XR_USE_GRAPHICS_API_D3D12))
#include "D3DGraphicsContext.h"
#endif
#include <openxr/openxr_platform.h>
#include <vector>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstddef>

struct SwapchainStagingSlotState
{
    std::atomic_bool inUse{false};
};

struct SwapchainSnapshotPoolState
{
    std::atomic_uint32_t inUse{0};
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

    static constexpr uint32_t SwapchainImageCount = 3;

#ifdef XR_USE_GRAPHICS_API_VULKAN
    struct VulkanFrameSource
    {
        std::shared_ptr<void> lifetime = {};
        VulkanGraphicsContext context = {};
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        size_t stagingSize = 0;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool fenceSubmitted = false;
        PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
        PFN_vkDestroyBuffer destroyBuffer = nullptr;
        PFN_vkFreeMemory freeMemory = nullptr;
        PFN_vkDestroyFence destroyFence = nullptr;
        PFN_vkWaitForFences waitForFences = nullptr;
        PFN_vkMapMemory mapMemory = nullptr;
        PFN_vkUnmapMemory unmapMemory = nullptr;
        PFN_vkInvalidateMappedMemoryRanges invalidateMappedMemoryRanges = nullptr;
    };
#endif

#ifdef XR_USE_GRAPHICS_API_OPENGL
    struct OpenGLFrameSource
    {
        std::vector<uint8_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
        GLenum format = GL_RGBA;
    };
#endif

#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(OXRSYS_USE_D3D12) || \
                        defined(XR_USE_GRAPHICS_API_D3D11) || defined(XR_USE_GRAPHICS_API_D3D12))
    struct D3D11FrameSource
    {
        std::shared_ptr<void> lifetime = {};
        ID3D11DeviceContext* immediateContext = nullptr;
        ID3D11Texture2D* stagingTexture = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct D3D12FrameSource
    {
        std::shared_ptr<void> lifetime = {};
        ID3D12Resource* sourceTexture = nullptr;
        ID3D12Resource* readbackBuffer = nullptr;
        ID3D12Fence* fence = nullptr;
        HANDLE fenceEvent = nullptr;
        uint64_t fenceValue = 0;
        bool fenceSubmitted = false;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 totalBytes = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0;
        uint32_t height = 0;
        ID3D12CommandAllocator* commandAllocator = nullptr;
        ID3D12GraphicsCommandList* commandList = nullptr;
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
    void InitOpenGL(const OpenGLGraphicsContext& openGLContext,
                    const XrSwapchainCreateInfo* createInfo);
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))
    void InitD3D11(const D3D11GraphicsContext& d3d11Context,
                   const XrSwapchainCreateInfo* createInfo);
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))
    void InitD3D12(const D3D12GraphicsContext& d3d12Context,
                   const XrSwapchainCreateInfo* createInfo);
#endif
    void InitMetalStaging(void* metalDevice);
#ifdef XR_USE_GRAPHICS_API_METAL
    XrResult EnumerateMetalImages(uint32_t imageCapacityInput,
                                  XrSwapchainImageBaseHeader* images) const;
    void DestroyMetalResources();
    void SnapshotMetalReleasedImage();
    void* GetLastReleasedMetalTextureSlice(uint32_t arrayIndex) const;
    FrameImageSource SnapshotMetalFrameImageSource(uint32_t arrayIndex) const;
    static void ReleaseMetalTextureSlice(void* textureSlice);
#endif
#ifdef XR_USE_GRAPHICS_API_VULKAN
    XrResult EnumerateVulkanImages(uint32_t imageCapacityInput,
                                   XrSwapchainImageBaseHeader* images) const;
    void DestroyVulkanResources();
    FrameImageSource SnapshotVulkanFrameImageSource(uint32_t arrayIndex) const;
    static void ReleaseVulkanFrameSource(VulkanFrameSource* source);
#endif
#ifdef XR_USE_GRAPHICS_API_OPENGL
    XrResult EnumerateOpenGLImages(uint32_t imageCapacityInput,
                                   XrSwapchainImageBaseHeader* images) const;
    void DestroyOpenGLResources();
    void SnapshotOpenGLReleasedImage(uint32_t releasedIndex);
    void ResolveReadyOpenGLSnapshots();
    FrameImageSource SnapshotOpenGLFrameImageSource(uint32_t arrayIndex) const;
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))
    XrResult EnumerateD3D11Images(uint32_t imageCapacityInput,
                                  XrSwapchainImageBaseHeader* images) const;
    void DestroyD3D11Resources();
    FrameImageSource SnapshotD3D11FrameImageSource(uint32_t arrayIndex) const;
    static void ReleaseD3D11FrameSource(D3D11FrameSource* source);
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))
    XrResult EnumerateD3D12Images(uint32_t imageCapacityInput,
                                  XrSwapchainImageBaseHeader* images) const;
    void DestroyD3D12Resources();
    FrameImageSource SnapshotD3D12FrameImageSource(uint32_t arrayIndex) const;
    static void ReleaseD3D12FrameSource(D3D12FrameSource* source);
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
    std::shared_ptr<SwapchainSnapshotPoolState> backendSnapshotPool_ =
        std::make_shared<SwapchainSnapshotPoolState>();

    // Vulkan resources (only used when graphicsApi_ == Vulkan)
    void* vkDevice_ = nullptr;
    VulkanGraphicsContext vulkanContext_ = {};
    std::vector<uint64_t> vkImages_;   // VkImage handles
    std::vector<uint64_t> vkMemories_; // VkDeviceMemory handles
    std::vector<FrameImageSource> lastVulkanSnapshots_;

    // OpenGL resources (only used when graphicsApi_ == OpenGL)
    OpenGLGraphicsContext openGLContext_ = {};
#ifdef XR_USE_GRAPHICS_API_OPENGL
    struct OpenGLReadbackSlot
    {
        GLuint pbo = 0;
        GLsync fence = nullptr;
        size_t size = 0;
        uint32_t arrayIndex = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        GLenum format = GL_RGBA;
        bool pending = false;
    };

    std::vector<GLuint> glTextures_;
    std::vector<OpenGLReadbackSlot> glReadbackSlots_;
    std::vector<uint32_t> glReadbackNextSlotByLayer_;
    std::vector<std::shared_ptr<OpenGLFrameSource>> lastOpenGLSnapshots_;
#endif

#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))
    D3D11GraphicsContext d3d11Context_ = {};
    std::vector<void*> d3d11Textures_;
    std::vector<FrameImageSource> lastD3D11Snapshots_;
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))
    D3D12GraphicsContext d3d12Context_ = {};
    std::vector<void*> d3d12Resources_;
    std::vector<FrameImageSource> lastD3D12Snapshots_;
#endif

    uint32_t nextAcquireIndex_ = 0;
    uint32_t lastReleasedIndex_ = 0;
    bool staticImageAcquired_ = false;
    bool hasReleasedImage_ = false;
    std::vector<ImageState> imageStates_;
    std::deque<uint32_t> acquiredImageOrder_;
    mutable std::mutex stateMutex_;
};
