// SPDX-License-Identifier: MPL-2.0

#include "Swapchain.h"
#include "Runtime.h"
#include "VulkanDispatch.h"
#include <spdlog/spdlog.h>

#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>

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

} // namespace

#ifdef XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>

// Device-level Vulkan function pointers (resolved per-device via vkGetDeviceProcAddr)
// All Vulkan calls go through function pointers — we do not link against the Vulkan loader.
struct VkDeviceFuncs
{
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkExportMetalObjectsEXT exportMetalObjects = nullptr;
};

static VkDeviceFuncs gDeviceFuncs;
static VkDevice gLastLoadedDevice = VK_NULL_HANDLE;

static void LoadDeviceFunctions(VkDevice device)
{
    if (device == gLastLoadedDevice && gDeviceFuncs.createImage)
    {
        return;
    }
    gLastLoadedDevice = device;

    // Resolve vkGetDeviceProcAddr through the global Vulkan dispatch (app's loader)
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = gVulkanDispatch.getDeviceProcAddr;
    if (!fpGetDeviceProcAddr)
    {
        spdlog::error("OXRSys: vkGetDeviceProcAddr not available");
        return;
    }

    auto get = [device, fpGetDeviceProcAddr](const char* name) {
        return fpGetDeviceProcAddr(device, name);
    };

    gDeviceFuncs.createImage = (PFN_vkCreateImage)get("vkCreateImage");
    gDeviceFuncs.destroyImage = (PFN_vkDestroyImage)get("vkDestroyImage");
    gDeviceFuncs.getImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)get("vkGetImageMemoryRequirements");
    gDeviceFuncs.allocateMemory = (PFN_vkAllocateMemory)get("vkAllocateMemory");
    gDeviceFuncs.freeMemory = (PFN_vkFreeMemory)get("vkFreeMemory");
    gDeviceFuncs.bindImageMemory = (PFN_vkBindImageMemory)get("vkBindImageMemory");
    gDeviceFuncs.exportMetalObjects = (PFN_vkExportMetalObjectsEXT)get("vkExportMetalObjectsEXT");

    spdlog::info("OXRSys: Loaded Vulkan device functions (exportMetalObjects={})",
                  gDeviceFuncs.exportMetalObjects ? "yes" : "no");
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
    // Use the instance-level function from the app's Vulkan dispatch
    VkPhysicalDeviceMemoryProperties memProps;
    if (gVulkanDispatch.getPhysicalDeviceMemoryProperties)
    {
        gVulkanDispatch.getPhysicalDeviceMemoryProperties(physDevice, &memProps);
    }
    else
    {
        spdlog::error("OXRSys: vkGetPhysicalDeviceMemoryProperties not available");
        return 0;
    }
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
    {
        if ((typeFilter & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    return 0;
}
#endif // XR_USE_GRAPHICS_API_VULKAN

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
    }
    else
    {
        InitMetal(graphicsContext.metalDevice, createInfo);
    }
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
        InitMetal(metalDevice, createInfo);
    }
}

// ============================================================================
// Metal initialization
// ============================================================================

void Swapchain::InitMetal(void* metalDevice, const XrSwapchainCreateInfo* createInfo)
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
    imageCount_ = (createInfo->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0 ? 1 : SwapchainImageCount;

    id<MTLDevice> device = (__bridge id<MTLDevice>)metalDevice;
    if (device == nil)
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
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

    textures_.resize(imageCount_);
    imageStates_.assign(imageCount_, ImageState::Available);
    for (uint32_t i = 0; i < imageCount_; i++)
    {
        id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
        textures_[i] = (void*)tex;
        if (tex == nil)
        {
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
        }
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

// ============================================================================
// Vulkan initialization (VkImage + MoltenVK MTLTexture extraction)
// ============================================================================

void Swapchain::InitVulkan(void* /*metalDevice*/, const VulkanGraphicsContext* vulkanContext,
                           const XrSwapchainCreateInfo* createInfo)
{
#ifdef XR_USE_GRAPHICS_API_VULKAN
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
    imageCount_ = (createInfo->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0 ? 1 : SwapchainImageCount;
    vulkanContext_ = vulkanContext;
    if (vulkanContext_ == nullptr || vulkanContext_->device == VK_NULL_HANDLE ||
        vulkanContext_->physicalDevice == VK_NULL_HANDLE)
    {
        spdlog::error("OXRSys: Missing Vulkan graphics context for swapchain creation");
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    VkDevice device = vulkanContext_->device;
    LoadDeviceFunctions(device);
    VkPhysicalDevice physDevice = vulkanContext_->physicalDevice;
    if (gDeviceFuncs.createImage == nullptr ||
        gDeviceFuncs.getImageMemoryRequirements == nullptr ||
        gDeviceFuncs.allocateMemory == nullptr ||
        gDeviceFuncs.bindImageMemory == nullptr)
    {
        spdlog::error("OXRSys: Missing Vulkan device functions for swapchain creation");
        initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    bool isDepth = IsDepthFormat(format_);
    bool hasExportMetalObjects = (gDeviceFuncs.exportMetalObjects != nullptr);

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

    // Tag images for Metal texture export if VK_EXT_metal_objects is available
    VkExportMetalObjectCreateInfoEXT exportCI{};
    if (hasExportMetalObjects)
    {
        exportCI.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT;
        exportCI.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT;
        imageCI.pNext = &exportCI;
    }

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
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            continue;
        }
        vkImages_[i] = reinterpret_cast<uint64_t>(image);

        // Allocate and bind memory
        VkMemoryRequirements memReqs;
        gDeviceFuncs.getImageMemoryRequirements(device, image, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VkDeviceMemory memory = VK_NULL_HANDLE;
        result = gDeviceFuncs.allocateMemory(device, &allocInfo, nullptr, &memory);
        if (result != VK_SUCCESS)
        {
            spdlog::error("OXRSys: vkAllocateMemory failed with {}", static_cast<int>(result));
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            continue;
        }
        vkMemories_[i] = reinterpret_cast<uint64_t>(memory);

        result = gDeviceFuncs.bindImageMemory(device, image, memory, 0);
        if (result != VK_SUCCESS)
        {
            spdlog::error("OXRSys: vkBindImageMemory failed with {}", static_cast<int>(result));
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            continue;
        }

        // Extract MTLTexture via VK_EXT_metal_objects for debug rendering
        if (hasExportMetalObjects && !isDepth)
        {
            VkExportMetalTextureInfoEXT texInfo{};
            texInfo.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT;
            texInfo.image = image;
            texInfo.plane = VK_IMAGE_ASPECT_COLOR_BIT;

            VkExportMetalObjectsInfoEXT objectsInfo{};
            objectsInfo.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
            objectsInfo.pNext = &texInfo;

            gDeviceFuncs.exportMetalObjects(device, &objectsInfo);

            if (texInfo.mtlTexture)
            {
                id<MTLTexture> mtl = texInfo.mtlTexture;
                textures_[i] = (void*)[mtl retain];
            }
        }
    }

    for (uint32_t i = 0; i < imageCount_; ++i)
    {
        if (vkImages_[i] == 0 || vkMemories_[i] == 0)
        {
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            break;
        }
    }

    Runtime::Get().RegisterHandle(handle_, this);
    spdlog::info("OXRSys: Vulkan swapchain created {}x{} format={} arraySize={} images={}",
                  width_, height_, format_, arraySize_, imageCount_);
#else
    spdlog::error("OXRSys: Vulkan support not compiled in");
    (void)createInfo;
    initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
    Runtime::Get().RegisterHandle(handle_, this);
#endif
}

// ============================================================================
// Destructor
// ============================================================================

Swapchain::~Swapchain()
{
#ifdef XR_USE_GRAPHICS_API_VULKAN
    if (graphicsApi_ == GraphicsApi::Vulkan && vulkanContext_ != nullptr &&
        vulkanContext_->device != VK_NULL_HANDLE)
    {
        VkDevice device = vulkanContext_->device;
        LoadDeviceFunctions(device);
        for (uint32_t i = 0; i < imageCount_; i++)
        {
            if (i < vkImages_.size() && vkImages_[i] && gDeviceFuncs.destroyImage)
            {
                gDeviceFuncs.destroyImage(device, reinterpret_cast<VkImage>(vkImages_[i]), nullptr);
            }
            if (i < vkMemories_.size() && vkMemories_[i] && gDeviceFuncs.freeMemory)
            {
                gDeviceFuncs.freeMemory(device, reinterpret_cast<VkDeviceMemory>(vkMemories_[i]), nullptr);
            }
        }
    }
#endif

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
    Runtime::Get().RemoveHandle(handle_);
}

// ============================================================================
// EnumerateImages — branch on graphics API
// ============================================================================

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

    if (graphicsApi_ == GraphicsApi::Metal)
    {
        auto* metalImages = reinterpret_cast<XrSwapchainImageMetalKHR*>(images);
        for (uint32_t i = 0; i < imageCount_; i++)
        {
            metalImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
            metalImages[i].next = nullptr;
            metalImages[i].texture = textures_[i];
        }
    }
    else
    {
#ifdef XR_USE_GRAPHICS_API_VULKAN
        auto* vulkanImages = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
        for (uint32_t i = 0; i < imageCount_; i++)
        {
            vulkanImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
            vulkanImages[i].next = nullptr;
            vulkanImages[i].image = reinterpret_cast<VkImage>(vkImages_[i]);
        }
#endif
    }

    return XR_SUCCESS;
}

// ============================================================================
// Acquire / Wait / Release — unchanged, same for both APIs
// ============================================================================

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

    if (staticImageAcquired_)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    if (acquiredImageOrder_.size() >= imageCount_)
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

    const bool waitEligible = std::all_of(acquiredImageOrder_.begin(),
                                          acquiredImageOrder_.end(),
                                          [this](uint32_t imageIndex) {
                                              return imageIndex < imageStates_.size() &&
                                                     imageStates_[imageIndex] == ImageState::Waited;
                                          });
    imageStates_[acquiredIndex] = ImageState::Acquired;
    acquiredImageOrder_.push_back(acquiredIndex);
    acquiredImageWaitEligible_.push_back(waitEligible);
    nextAcquireIndex_ = (acquiredIndex + 1) % imageCount_;
    staticImageAcquired_ = (imageCount_ == 1);
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

    for (size_t i = 0; i < acquiredImageOrder_.size() && i < acquiredImageWaitEligible_.size(); ++i)
    {
        const uint32_t waitIndex = acquiredImageOrder_[i];
        if (acquiredImageWaitEligible_[i] && waitIndex < imageStates_.size() &&
            imageStates_[waitIndex] == ImageState::Acquired)
        {
            imageStates_[waitIndex] = ImageState::Waited;
            acquiredImageWaitEligible_[i] = false;
            return XR_SUCCESS;
        }
    }

    return XR_ERROR_CALL_ORDER_INVALID;
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
    acquiredImageWaitEligible_.pop_front();
    if (!acquiredImageWaitEligible_.empty())
    {
        acquiredImageWaitEligible_.front() = true;
    }

    hasSnapshot_ = false;
    lastSnapshotValue_ = 0;
    lastSnapshotLease_.reset();

    if (graphicsApi_ == GraphicsApi::Metal && imageCount_ > 1 &&
        !stagingSlots_.empty() && snapshotEvent_ != nullptr && metalCommandQueue_ != nullptr)
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

    return XR_SUCCESS;
}

// ============================================================================
// GetLastReleasedTexture — always returns MTLTexture* (for debug renderer)
// ============================================================================

void* Swapchain::GetLastReleasedTexture() const
{
    if (textures_.empty())
    {
        return nullptr;
    }
    return textures_[lastReleasedIndex_];
}

void* Swapchain::GetLastReleasedTextureSlice(uint32_t arrayIndex) const
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

FrameImageSource Swapchain::GetLastReleasedFrameImageSource(uint32_t arrayIndex) const
{
    std::scoped_lock lock(stateMutex_);

    if (!hasReleasedImage_ || textures_.empty())
    {
        return {};
    }

    if (arrayIndex >= arraySize_)
    {
        spdlog::warn("OXRSys: arrayIndex {} >= arraySize {}", arrayIndex, arraySize_);
        return {};
    }

    if (graphicsApi_ == GraphicsApi::Metal && imageCount_ > 1)
    {
        if (stagingSlots_.empty())
        {
            return {};
        }
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

void Swapchain::ReleaseTextureSlice(void* textureSlice)
{
    if (textureSlice)
    {
        [(id<MTLTexture>)textureSlice release];
    }
}

#ifdef XR_USE_GRAPHICS_API_VULKAN
void Swapchain::ReleaseVulkanFrameSource(VulkanFrameSource* source)
{
    delete source;
}
#endif

bool Swapchain::HasReleasedImage() const
{
    std::scoped_lock lock(stateMutex_);
    return hasReleasedImage_ && imageStates_[lastReleasedIndex_] == ImageState::Available;
}
