// SPDX-License-Identifier: MPL-2.0

#include "Swapchain.h"
#include "Runtime.h"

#include <spdlog/spdlog.h>

Swapchain::Swapchain(const GraphicsContext& graphicsContext, const XrSwapchainCreateInfo* createInfo)
    : device_(graphicsContext.metalDevice),
      metalCommandQueue_(graphicsContext.metalCommandQueue),
      graphicsApi_(graphicsContext.api)
{
#ifdef XR_USE_GRAPHICS_API_METAL
    if (graphicsContext.api == GraphicsApi::Metal)
    {
        InitMetal(graphicsContext.metalDevice, createInfo);
        return;
    }
#endif
    if (graphicsContext.api == GraphicsApi::Vulkan)
    {
        InitVulkan(graphicsContext.metalDevice, graphicsContext.vulkan, createInfo);
        return;
    }
#ifdef XR_USE_GRAPHICS_API_OPENGL
    if (graphicsContext.api == GraphicsApi::OpenGL)
    {
        InitOpenGL(graphicsContext.openGL, createInfo);
        return;
    }
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))
    if (graphicsContext.api == GraphicsApi::D3D11)
    {
        InitD3D11(graphicsContext.d3d11, createInfo);
        return;
    }
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))
    if (graphicsContext.api == GraphicsApi::D3D12)
    {
        InitD3D12(graphicsContext.d3d12, createInfo);
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
    spdlog::error("OXRSys: unsupported swapchain graphics API in this runtime build");
}

#ifndef XR_USE_GRAPHICS_API_METAL
void Swapchain::InitMetal(void* /*metalDevice*/, const XrSwapchainCreateInfo* /*createInfo*/)
{
    initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
    spdlog::error("OXRSys: InitMetal called in a non-Apple runtime build");
}

void Swapchain::InitMetalStaging(void* /*metalDevice*/)
{
}
#endif

Swapchain::~Swapchain()
{
#ifdef XR_USE_GRAPHICS_API_METAL
    if (graphicsApi_ == GraphicsApi::Metal)
    {
        DestroyMetalResources();
    }
#endif
#ifdef XR_USE_GRAPHICS_API_VULKAN
    if (graphicsApi_ == GraphicsApi::Vulkan)
    {
        DestroyVulkanResources();
    }
#endif
#ifdef XR_USE_GRAPHICS_API_OPENGL
    if (graphicsApi_ == GraphicsApi::OpenGL)
    {
        DestroyOpenGLResources();
    }
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))
    if (graphicsApi_ == GraphicsApi::D3D11)
    {
        DestroyD3D11Resources();
    }
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))
    if (graphicsApi_ == GraphicsApi::D3D12)
    {
        DestroyD3D12Resources();
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
    if (images == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

#ifdef XR_USE_GRAPHICS_API_METAL
    if (graphicsApi_ == GraphicsApi::Metal)
    {
        return EnumerateMetalImages(imageCapacityInput, images);
    }
#endif
#ifdef XR_USE_GRAPHICS_API_VULKAN
    if (graphicsApi_ == GraphicsApi::Vulkan)
    {
        return EnumerateVulkanImages(imageCapacityInput, images);
    }
#endif
#ifdef XR_USE_GRAPHICS_API_OPENGL
    if (graphicsApi_ == GraphicsApi::OpenGL)
    {
        return EnumerateOpenGLImages(imageCapacityInput, images);
    }
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))
    if (graphicsApi_ == GraphicsApi::D3D11)
    {
        return EnumerateD3D11Images(imageCapacityInput, images);
    }
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))
    if (graphicsApi_ == GraphicsApi::D3D12)
    {
        return EnumerateD3D12Images(imageCapacityInput, images);
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

    uint32_t releaseIndex = 0;
    {
        std::scoped_lock lock(stateMutex_);
        if (acquiredImageOrder_.empty())
        {
            return XR_ERROR_CALL_ORDER_INVALID;
        }

        releaseIndex = acquiredImageOrder_.front();
        if (imageStates_[releaseIndex] != ImageState::Waited)
        {
            return XR_ERROR_CALL_ORDER_INVALID;
        }

        lastReleasedIndex_ = releaseIndex;
        imageStates_[releaseIndex] = ImageState::Available;
        hasReleasedImage_ = true;
        acquiredImageOrder_.pop_front();
    }

#ifdef XR_USE_GRAPHICS_API_METAL
    if (graphicsApi_ == GraphicsApi::Metal)
    {
        SnapshotMetalReleasedImage();
    }
#endif
#ifdef XR_USE_GRAPHICS_API_VULKAN
    if (graphicsApi_ == GraphicsApi::Vulkan)
    {
        lastVulkanSnapshots_.assign(arraySize_, {});
        for (uint32_t arrayIndex = 0; arrayIndex < arraySize_; ++arrayIndex)
        {
            lastVulkanSnapshots_[arrayIndex] = SnapshotVulkanFrameImageSource(arrayIndex);
        }
    }
#endif
#ifdef XR_USE_GRAPHICS_API_OPENGL
    if (graphicsApi_ == GraphicsApi::OpenGL)
    {
        SnapshotOpenGLReleasedImage(releaseIndex);
    }
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))
    if (graphicsApi_ == GraphicsApi::D3D11)
    {
        lastD3D11Snapshots_.assign(arraySize_, {});
        for (uint32_t arrayIndex = 0; arrayIndex < arraySize_; ++arrayIndex)
        {
            lastD3D11Snapshots_[arrayIndex] = SnapshotD3D11FrameImageSource(arrayIndex);
        }
    }
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))
    if (graphicsApi_ == GraphicsApi::D3D12)
    {
        lastD3D12Snapshots_.assign(arraySize_, {});
        for (uint32_t arrayIndex = 0; arrayIndex < arraySize_; ++arrayIndex)
        {
            lastD3D12Snapshots_[arrayIndex] = SnapshotD3D12FrameImageSource(arrayIndex);
        }
    }
#endif

    return XR_SUCCESS;
}

void* Swapchain::GetLastReleasedTexture() const
{
#ifdef XR_USE_GRAPHICS_API_METAL
    if (graphicsApi_ == GraphicsApi::Metal)
    {
        if (textures_.empty())
        {
            return nullptr;
        }
        return textures_[lastReleasedIndex_];
    }
#endif
    if (graphicsApi_ == GraphicsApi::Vulkan)
    {
#if defined(__APPLE__) && defined(XR_USE_GRAPHICS_API_METAL)
        if (!textures_.empty() && textures_[lastReleasedIndex_] != nullptr)
        {
            return textures_[lastReleasedIndex_];
        }
#endif
        if (vkImages_.empty())
        {
            return nullptr;
        }
        return reinterpret_cast<void*>(vkImages_[lastReleasedIndex_]);
    }
#ifdef XR_USE_GRAPHICS_API_OPENGL
    if (graphicsApi_ == GraphicsApi::OpenGL)
    {
        if (glTextures_.empty())
        {
            return nullptr;
        }
        return reinterpret_cast<void*>(static_cast<uintptr_t>(glTextures_[lastReleasedIndex_]));
    }
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))
    if (graphicsApi_ == GraphicsApi::D3D11)
    {
        if (d3d11Textures_.empty())
        {
            return nullptr;
        }
        return d3d11Textures_[lastReleasedIndex_];
    }
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))
    if (graphicsApi_ == GraphicsApi::D3D12)
    {
        if (d3d12Resources_.empty())
        {
            return nullptr;
        }
        return d3d12Resources_[lastReleasedIndex_];
    }
#endif
    return nullptr;
}

void* Swapchain::GetLastReleasedTextureSlice(uint32_t arrayIndex) const
{
    if (arrayIndex >= arraySize_)
    {
        spdlog::warn("OXRSys: arrayIndex {} >= arraySize {}", arrayIndex, arraySize_);
        return nullptr;
    }
#ifdef XR_USE_GRAPHICS_API_METAL
    if (graphicsApi_ == GraphicsApi::Metal ||
        (graphicsApi_ == GraphicsApi::Vulkan && !textures_.empty()))
    {
        return GetLastReleasedMetalTextureSlice(arrayIndex);
    }
#endif
    return GetLastReleasedTexture();
}

FrameImageSource Swapchain::GetLastReleasedFrameImageSource(uint32_t arrayIndex) const
{
    std::scoped_lock lock(stateMutex_);
    if (!hasReleasedImage_ || arrayIndex >= arraySize_)
    {
        return {};
    }

#ifdef XR_USE_GRAPHICS_API_METAL
    if (graphicsApi_ == GraphicsApi::Metal)
    {
        return SnapshotMetalFrameImageSource(arrayIndex);
    }
#endif
#ifdef XR_USE_GRAPHICS_API_VULKAN
    if (graphicsApi_ == GraphicsApi::Vulkan)
    {
        if (arrayIndex >= lastVulkanSnapshots_.size())
        {
            return {};
        }
        return lastVulkanSnapshots_[arrayIndex];
    }
#endif
#ifdef XR_USE_GRAPHICS_API_OPENGL
    if (graphicsApi_ == GraphicsApi::OpenGL)
    {
        return SnapshotOpenGLFrameImageSource(arrayIndex);
    }
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))
    if (graphicsApi_ == GraphicsApi::D3D11)
    {
        if (arrayIndex >= lastD3D11Snapshots_.size())
        {
            return {};
        }
        return lastD3D11Snapshots_[arrayIndex];
    }
#endif
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))
    if (graphicsApi_ == GraphicsApi::D3D12)
    {
        if (arrayIndex >= lastD3D12Snapshots_.size())
        {
            return {};
        }
        return lastD3D12Snapshots_[arrayIndex];
    }
#endif
    return {};
}

void Swapchain::ReleaseTextureSlice(void* textureSlice)
{
#ifdef XR_USE_GRAPHICS_API_METAL
    ReleaseMetalTextureSlice(textureSlice);
#else
    (void)textureSlice;
#endif
}

bool Swapchain::HasReleasedImage() const
{
    std::scoped_lock lock(stateMutex_);
    return hasReleasedImage_ && imageStates_[lastReleasedIndex_] == ImageState::Available;
}
