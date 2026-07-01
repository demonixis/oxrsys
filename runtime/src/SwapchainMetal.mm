// SPDX-License-Identifier: MPL-2.0

#include "Swapchain.h"
#include "Runtime.h"
#include <spdlog/spdlog.h>

#import <Metal/Metal.h>
#include <atomic>
#include <utility>

#include "OpenXRPlatform.h"

namespace
{

constexpr uint32_t kMetalStagingImageCount = Swapchain::SwapchainImageCount + 1;
static std::atomic<uint64_t> gMetalSnapshotValue{0};

void ReleaseMetalObject(void* object)
{
    if (object != nullptr)
    {
        [(id)object release];
    }
}

std::shared_ptr<void> RetainMetalObject(id object)
{
    if (object == nil)
    {
        return {};
    }
    return std::shared_ptr<void>((void*)[object retain], ReleaseMetalObject);
}

std::shared_ptr<void> AdoptMetalObject(id object)
{
    if (object == nil)
    {
        return {};
    }
    return std::shared_ptr<void>((void*)object, ReleaseMetalObject);
}

std::shared_ptr<void> MakeStagingLease(const std::shared_ptr<SwapchainStagingSlotState>& state)
{
    if (!state)
    {
        return {};
    }

    state->inUse.store(true, std::memory_order_release);
    return std::shared_ptr<void>(state.get(), [state](void*) {
        state->inUse.store(false, std::memory_order_release);
    });
}

FrameImageSource MakeMetalFrameImageSource(id<MTLTexture> texture,
                                           uint32_t arraySize,
                                           uint32_t arrayIndex,
                                           const std::shared_ptr<void>& lifetime,
                                           const std::shared_ptr<void>& waitEvent,
                                           uint64_t waitValue)
{
    FrameImageSource source = {};
    source.api = GraphicsApi::Metal;
    source.lifetime = lifetime;
    source.sync.api = GraphicsApi::Metal;
    source.sync.waitObject = waitEvent;
    source.sync.waitValue = waitValue;

    if (texture == nil)
    {
        return {};
    }

    if (arraySize <= 1 || texture.textureType != MTLTextureType2DArray)
    {
        source.image = RetainMetalObject(texture);
        return source;
    }

    if (arrayIndex >= texture.arrayLength)
    {
        spdlog::warn("OXRSys: arrayIndex {} >= arrayLength {}", arrayIndex, (uint32_t)texture.arrayLength);
        return {};
    }

    id<MTLTexture> sliceView = [texture newTextureViewWithPixelFormat:texture.pixelFormat
                                                          textureType:MTLTextureType2D
                                                               levels:NSMakeRange(0, 1)
                                                               slices:NSMakeRange(arrayIndex, 1)];
    source.image = AdoptMetalObject(sliceView);
    return source;
}

bool IsSupportedMetalFormat(int64_t format)
{
    switch (static_cast<MTLPixelFormat>(format))
    {
        case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatRGBA8Unorm_sRGB:
        case MTLPixelFormatRGBA8Unorm:
        case MTLPixelFormatDepth32Float:
        case MTLPixelFormatDepth32Float_Stencil8:
            return true;
        default:
            return false;
    }
}

} // namespace

extern "C" void* OxrsysRetainMetalObjectForSwapchain(void* object)
{
    if (object == nullptr)
    {
        return nullptr;
    }
    return (void*)[(id)object retain];
}

// ============================================================================
// Metal initialization
// ============================================================================

void Swapchain::InitMetal(void* metalDevice, const XrSwapchainCreateInfo* createInfo)
{
    if (metalDevice == nullptr || createInfo == nullptr)
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    width_ = createInfo->width;
    height_ = createInfo->height;
    format_ = createInfo->format;
    arraySize_ = createInfo->arraySize > 0 ? createInfo->arraySize : 1;
    imageCount_ = (createInfo->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0 ? 1 : SwapchainImageCount;

    if (!IsSupportedMetalFormat(format_))
    {
        initializationResult_ = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: unsupported Metal swapchain format {}", format_);
        return;
    }

    id<MTLDevice> device = (__bridge id<MTLDevice>)metalDevice;

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)format_
                                                                                    width:width_
                                                                                   height:height_
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;
    if (arraySize_ > 1)
    {
        desc.textureType = MTLTextureType2DArray;
        desc.arrayLength = arraySize_;
    }

    textures_.resize(imageCount_);
    imageStates_.assign(imageCount_, ImageState::Available);
    bool textureCreationFailed = false;
    for (uint32_t i = 0; i < imageCount_; i++)
    {
        id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
        if (tex == nil)
        {
            textureCreationFailed = true;
        }
        textures_[i] = (void*)tex;
    }

    if (textureCreationFailed)
    {
        initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: Metal swapchain texture creation failed format={}", format_);
        return;
    }

    InitMetalStaging(metalDevice);

    Runtime::Get().RegisterHandle(handle_, this);
    spdlog::info("OXRSys: Metal swapchain created {}x{} format={} arraySize={} images={}",
                  width_, height_, format_, arraySize_, imageCount_);
}

void Swapchain::InitMetalStaging(void* metalDevice)
{
    if (imageCount_ <= 1 || metalCommandQueue_ == nullptr)
    {
        return;
    }

    id<MTLDevice> device = (__bridge id<MTLDevice>)metalDevice;
    if (device == nil)
    {
        return;
    }

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)format_
                                                                                    width:width_
                                                                                   height:height_
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;
    if (arraySize_ > 1)
    {
        desc.textureType = MTLTextureType2DArray;
        desc.arrayLength = arraySize_;
    }

    snapshotEvent_ = (void*)[device newSharedEvent];
    if (snapshotEvent_ == nullptr)
    {
        spdlog::warn("OXRSys: Metal swapchain staging disabled because MTLSharedEvent creation failed");
        return;
    }

    stagingSlots_.resize(kMetalStagingImageCount);
    for (uint32_t i = 0; i < kMetalStagingImageCount; ++i)
    {
        id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
        stagingSlots_[i].texture = (void*)tex;
        stagingSlots_[i].state = std::make_shared<SwapchainStagingSlotState>();
    }

    spdlog::info("OXRSys: Metal swapchain staging enabled slots={}", kMetalStagingImageCount);
}

void Swapchain::DestroyMetalResources()
{
    lastSnapshotLease_.reset();

    for (const auto& slot : stagingSlots_)
    {
        if (slot.texture)
        {
            [(id<MTLTexture>)slot.texture release];
        }
    }
    stagingSlots_.clear();

    if (snapshotEvent_ != nullptr)
    {
        [(id<MTLSharedEvent>)snapshotEvent_ release];
        snapshotEvent_ = nullptr;
    }

    for (auto* tex : textures_)
    {
        if (tex)
        {
            [(id<MTLTexture>)tex release];
        }
    }
    textures_.clear();
}

// ============================================================================
// Metal images
// ============================================================================

XrResult Swapchain::EnumerateMetalImages(uint32_t /*imageCapacityInput*/,
                                         XrSwapchainImageBaseHeader* images) const
{
    auto* metalImages = reinterpret_cast<XrSwapchainImageMetalKHR*>(images);
    for (uint32_t i = 0; i < imageCount_; i++)
    {
        metalImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
        metalImages[i].next = nullptr;
        metalImages[i].texture = textures_[i];
    }
    return XR_SUCCESS;
}

// ============================================================================
// Release-time snapshot
// ============================================================================

void Swapchain::SnapshotMetalReleasedImage()
{
    hasSnapshot_ = false;
    lastSnapshotValue_ = 0;
    lastSnapshotLease_.reset();

    if (imageCount_ > 1 && !stagingSlots_.empty() &&
        snapshotEvent_ != nullptr && metalCommandQueue_ != nullptr)
    {
        uint32_t stagingIndex = static_cast<uint32_t>(stagingSlots_.size());
        for (uint32_t i = 0; i < stagingSlots_.size(); ++i)
        {
            uint32_t candidate = (nextStagingIndex_ + i) % static_cast<uint32_t>(stagingSlots_.size());
            const auto& slot = stagingSlots_[candidate];
            if (slot.state && !slot.state->inUse.load(std::memory_order_acquire))
            {
                stagingIndex = candidate;
                break;
            }
        }

        if (stagingIndex < stagingSlots_.size())
        {
            StagingSlot& stagingSlot = stagingSlots_[stagingIndex];
            std::shared_ptr<void> lease = MakeStagingLease(stagingSlot.state);
            id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)metalCommandQueue_;
            id<MTLTexture> src = (__bridge id<MTLTexture>)textures_[lastReleasedIndex_];
            id<MTLTexture> dst = (__bridge id<MTLTexture>)stagingSlot.texture;
            id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)snapshotEvent_;
            id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = commandBuffer != nil ? [commandBuffer blitCommandEncoder] : nil;
            if (lease && commandBuffer != nil && blit != nil && src != nil && dst != nil && event != nil)
            {
                const uint64_t signalValue = gMetalSnapshotValue.fetch_add(1, std::memory_order_relaxed) + 1;
                [blit copyFromTexture:src toTexture:dst];
                [blit endEncoding];
                [commandBuffer encodeSignalEvent:event value:signalValue];
                [commandBuffer commit];

                lastSnapshotIndex_ = stagingIndex;
                lastSnapshotValue_ = signalValue;
                lastSnapshotLease_ = std::move(lease);
                hasSnapshot_ = true;
                nextStagingIndex_ = (stagingIndex + 1) % static_cast<uint32_t>(stagingSlots_.size());
            }
        }
        else
        {
            spdlog::debug("OXRSys: Metal swapchain staging pool is full; streaming snapshot unavailable");
        }
    }
}

void* Swapchain::GetLastReleasedMetalTextureSlice(uint32_t arrayIndex) const
{
    if (textures_.empty())
    {
        return nullptr;
    }

    id<MTLTexture> tex = (id<MTLTexture>)textures_[lastReleasedIndex_];
    if (!tex)
    {
        return nullptr;
    }

    // For non-array textures, return as-is (retain for caller symmetry)
    if (arraySize_ <= 1 || tex.textureType != MTLTextureType2DArray)
    {
        return (void*)[tex retain];
    }

    // Create a 2D texture view into a specific array slice
    if (arrayIndex >= tex.arrayLength)
    {
        spdlog::warn("OXRSys: arrayIndex {} >= arrayLength {}", arrayIndex, (uint32_t)tex.arrayLength);
        return nullptr;
    }

    id<MTLTexture> sliceView = [tex newTextureViewWithPixelFormat:tex.pixelFormat
                                                      textureType:MTLTextureType2D
                                                           levels:NSMakeRange(0, 1)
                                                           slices:NSMakeRange(arrayIndex, 1)];
    return (void*)sliceView; // caller owns this reference
}

FrameImageSource Swapchain::SnapshotMetalFrameImageSource(uint32_t arrayIndex) const
{
    if (!hasReleasedImage_ || textures_.empty())
    {
        return {};
    }

    if (arrayIndex >= arraySize_)
    {
        spdlog::warn("OXRSys: arrayIndex {} >= arraySize {}", arrayIndex, arraySize_);
        return {};
    }

    if (imageCount_ > 1 && !stagingSlots_.empty())
    {
        if (!hasSnapshot_ || lastSnapshotIndex_ >= stagingSlots_.size() ||
            stagingSlots_[lastSnapshotIndex_].texture == nullptr)
        {
            return {};
        }

        std::shared_ptr<void> waitEvent = RetainMetalObject((id)snapshotEvent_);
        id<MTLTexture> texture = (__bridge id<MTLTexture>)stagingSlots_[lastSnapshotIndex_].texture;
        return MakeMetalFrameImageSource(
            texture, arraySize_, arrayIndex, lastSnapshotLease_, waitEvent, lastSnapshotValue_);
    }

    id<MTLTexture> texture = (__bridge id<MTLTexture>)textures_[lastReleasedIndex_];
    return MakeMetalFrameImageSource(texture, arraySize_, arrayIndex, {}, {}, 0);
}

void Swapchain::ReleaseMetalTextureSlice(void* textureSlice)
{
    if (textureSlice)
    {
        [(id<MTLTexture>)textureSlice release];
    }
}
