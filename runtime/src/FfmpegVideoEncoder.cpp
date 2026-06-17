// SPDX-License-Identifier: MPL-2.0

#include "VideoEncoder.h"
#include "Config.h"
#include "Swapchain.h"
#include "VulkanDispatch.h"
#include "VulkanGraphicsContext.h"
#if defined(_WIN32)
#include "D3DGraphicsContext.h"
#endif

#include <spdlog/spdlog.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{

using Clock = std::chrono::steady_clock;

double ToMilliseconds(Clock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

AVCodecContext* CodecContext(void* ptr)
{
    return static_cast<AVCodecContext*>(ptr);
}

AVFrame* Frame(void* ptr)
{
    return static_cast<AVFrame*>(ptr);
}

AVPacket* Packet(void* ptr)
{
    return static_cast<AVPacket*>(ptr);
}

bool CodecSupportsPixelFormat(const AVCodec* codec, AVPixelFormat format)
{
    if (codec == nullptr || codec->pix_fmts == nullptr)
    {
        return true;
    }

    for (const AVPixelFormat* pixelFormat = codec->pix_fmts;
         *pixelFormat != AV_PIX_FMT_NONE;
         ++pixelFormat)
    {
        if (*pixelFormat == format)
        {
            return true;
        }
    }
    return false;
}

AVPixelFormat ChooseEncoderPixelFormat(const AVCodec* codec)
{
    if (CodecSupportsPixelFormat(codec, AV_PIX_FMT_NV12))
    {
        return AV_PIX_FMT_NV12;
    }
    if (CodecSupportsPixelFormat(codec, AV_PIX_FMT_YUV420P))
    {
        return AV_PIX_FMT_YUV420P;
    }
    if (codec != nullptr && codec->pix_fmts != nullptr && codec->pix_fmts[0] != AV_PIX_FMT_NONE)
    {
        return codec->pix_fmts[0];
    }
    return AV_PIX_FMT_YUV420P;
}

const AVCodec* FindNamedEncoder(const char* name)
{
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    return (codec != nullptr && av_codec_is_encoder(codec) != 0) ? codec : nullptr;
}

bool IsMediaFoundationEncoder(const AVCodec* codec)
{
    return codec != nullptr && codec->name != nullptr &&
           std::strcmp(codec->name, "hevc_mf") == 0;
}

const char* FfmpegErrorString(int error)
{
    static thread_local char buffer[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

const AVCodec* SelectHevcEncoder()
{
#if defined(_WIN32)
    if (const AVCodec* codec = FindNamedEncoder("hevc_mf"))
    {
        return codec;
    }
#endif

    const AVCodec* fallback = nullptr;
    void* iterator = nullptr;
    while (const AVCodec* codec = av_codec_iterate(&iterator))
    {
        if (av_codec_is_encoder(codec) == 0 || codec->type != AVMEDIA_TYPE_VIDEO ||
            codec->id != AV_CODEC_ID_HEVC)
        {
            continue;
        }

        const char* name = codec->name != nullptr ? codec->name : "";
        if (std::strcmp(name, "hevc_d3d12va") == 0)
        {
            fallback = fallback == nullptr ? codec : fallback;
            continue;
        }

        if (CodecSupportsPixelFormat(codec, AV_PIX_FMT_NV12) ||
            CodecSupportsPixelFormat(codec, AV_PIX_FMT_YUV420P))
        {
            return codec;
        }
        fallback = fallback == nullptr ? codec : fallback;
    }

    return fallback;
}

struct VulkanEncoderState
{
    GraphicsApi graphicsApi = GraphicsApi::Vulkan;
    VulkanGraphicsContext context = {};
#if defined(_WIN32)
    D3D11GraphicsContext d3d11Context = {};
    ID3D11Texture2D* d3d11StagingTexture = nullptr;
    DXGI_FORMAT d3d11StagingFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t d3d11StagingWidth = 0;
    uint32_t d3d11StagingHeight = 0;

    D3D12GraphicsContext d3d12Context = {};
    ID3D12CommandAllocator* d3d12CommandAllocator = nullptr;
    ID3D12GraphicsCommandList* d3d12CommandList = nullptr;
    ID3D12Fence* d3d12Fence = nullptr;
    HANDLE d3d12FenceEvent = nullptr;
    uint64_t d3d12FenceValue = 0;
    ID3D12Resource* d3d12ReadbackBuffer = nullptr;
    size_t d3d12ReadbackSize = 0;
    D3D12_COMMAND_LIST_TYPE d3d12CommandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;
#endif
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    size_t stagingSize = 0;
    SwsContext* eyeScaleContext = nullptr;
    SwsContext* rgbaToYuvContext = nullptr;
    std::vector<uint8_t> stagingReadback;
    std::vector<uint8_t> stereoRgba;

    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkResetCommandPool resetCommandPool = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
};

VulkanEncoderState* State(void* ptr)
{
    return static_cast<VulkanEncoderState*>(ptr);
}

std::optional<AVPixelFormat> AvPixelFormatForVulkanFormat(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return AV_PIX_FMT_RGBA;
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return AV_PIX_FMT_BGRA;
        default:
            return std::nullopt;
    }
}

#if defined(_WIN32)
std::optional<AVPixelFormat> AvPixelFormatForDxgiFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return AV_PIX_FMT_RGBA;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return AV_PIX_FMT_BGRA;
        default:
            return std::nullopt;
    }
}

void ReleaseD3D11StagingTexture(VulkanEncoderState& state)
{
    if (state.d3d11StagingTexture != nullptr)
    {
        state.d3d11StagingTexture->Release();
        state.d3d11StagingTexture = nullptr;
    }
    state.d3d11StagingFormat = DXGI_FORMAT_UNKNOWN;
    state.d3d11StagingWidth = 0;
    state.d3d11StagingHeight = 0;
}

bool EnsureD3D11StagingTexture(VulkanEncoderState& state,
                               const Swapchain::D3D11FrameSource& source)
{
    if (state.d3d11StagingTexture != nullptr &&
        state.d3d11StagingFormat == source.format &&
        state.d3d11StagingWidth == source.width &&
        state.d3d11StagingHeight == source.height)
    {
        return true;
    }

    ReleaseD3D11StagingTexture(state);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = source.width;
    desc.Height = source.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = source.format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT result = state.d3d11Context.device->CreateTexture2D(
        &desc, nullptr, &state.d3d11StagingTexture);
    if (FAILED(result))
    {
        return false;
    }

    state.d3d11StagingFormat = source.format;
    state.d3d11StagingWidth = source.width;
    state.d3d11StagingHeight = source.height;
    return true;
}

bool InitializeD3D12State(VulkanEncoderState& state)
{
    if (state.d3d12Context.device == nullptr || state.d3d12Context.queue == nullptr)
    {
        return false;
    }

    state.d3d12CommandListType = state.d3d12Context.queue->GetDesc().Type;
    if (state.d3d12CommandListType != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        state.d3d12CommandListType != D3D12_COMMAND_LIST_TYPE_COPY)
    {
        return false;
    }

    if (FAILED(state.d3d12Context.device->CreateCommandAllocator(
            state.d3d12CommandListType,
            __uuidof(ID3D12CommandAllocator),
            reinterpret_cast<void**>(&state.d3d12CommandAllocator))))
    {
        return false;
    }

    if (FAILED(state.d3d12Context.device->CreateCommandList(
            0,
            state.d3d12CommandListType,
            state.d3d12CommandAllocator,
            nullptr,
            __uuidof(ID3D12GraphicsCommandList),
            reinterpret_cast<void**>(&state.d3d12CommandList))))
    {
        return false;
    }
    state.d3d12CommandList->Close();

    if (FAILED(state.d3d12Context.device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            __uuidof(ID3D12Fence),
            reinterpret_cast<void**>(&state.d3d12Fence))))
    {
        return false;
    }

    state.d3d12FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return state.d3d12FenceEvent != nullptr;
}

void ReleaseD3D12ReadbackBuffer(VulkanEncoderState& state)
{
    if (state.d3d12ReadbackBuffer != nullptr)
    {
        state.d3d12ReadbackBuffer->Release();
        state.d3d12ReadbackBuffer = nullptr;
    }
    state.d3d12ReadbackSize = 0;
}

bool EnsureD3D12ReadbackBuffer(VulkanEncoderState& state, size_t size)
{
    if (state.d3d12ReadbackBuffer != nullptr && state.d3d12ReadbackSize >= size)
    {
        return true;
    }

    ReleaseD3D12ReadbackBuffer(state);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT result = state.d3d12Context.device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        __uuidof(ID3D12Resource),
        reinterpret_cast<void**>(&state.d3d12ReadbackBuffer));
    if (FAILED(result))
    {
        return false;
    }
    state.d3d12ReadbackSize = size;
    return true;
}
#endif

bool LoadVulkanEncoderFunctions(VulkanEncoderState& state)
{
    if (gVulkanDispatch.getDeviceProcAddr == nullptr || state.context.device == VK_NULL_HANDLE)
    {
        return false;
    }

    auto get = [&state](const char* name) {
        return gVulkanDispatch.getDeviceProcAddr(state.context.device, name);
    };

    state.createCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(get("vkCreateCommandPool"));
    state.destroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(get("vkDestroyCommandPool"));
    state.allocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(get("vkAllocateCommandBuffers"));
    state.resetCommandPool = reinterpret_cast<PFN_vkResetCommandPool>(get("vkResetCommandPool"));
    state.createFence = reinterpret_cast<PFN_vkCreateFence>(get("vkCreateFence"));
    state.destroyFence = reinterpret_cast<PFN_vkDestroyFence>(get("vkDestroyFence"));
    state.resetFences = reinterpret_cast<PFN_vkResetFences>(get("vkResetFences"));
    state.waitForFences = reinterpret_cast<PFN_vkWaitForFences>(get("vkWaitForFences"));
    state.beginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get("vkBeginCommandBuffer"));
    state.endCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get("vkEndCommandBuffer"));
    state.cmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(get("vkCmdPipelineBarrier"));
    state.cmdCopyImageToBuffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(get("vkCmdCopyImageToBuffer"));
    state.queueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(get("vkQueueSubmit"));
    state.createBuffer = reinterpret_cast<PFN_vkCreateBuffer>(get("vkCreateBuffer"));
    state.destroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(get("vkDestroyBuffer"));
    state.getBufferMemoryRequirements =
        reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get("vkGetBufferMemoryRequirements"));
    state.allocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(get("vkAllocateMemory"));
    state.freeMemory = reinterpret_cast<PFN_vkFreeMemory>(get("vkFreeMemory"));
    state.bindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(get("vkBindBufferMemory"));
    state.mapMemory = reinterpret_cast<PFN_vkMapMemory>(get("vkMapMemory"));
    state.unmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(get("vkUnmapMemory"));

    return state.createCommandPool && state.destroyCommandPool && state.allocateCommandBuffers &&
           state.resetCommandPool && state.createFence && state.destroyFence &&
           state.resetFences && state.waitForFences && state.beginCommandBuffer &&
           state.endCommandBuffer && state.cmdPipelineBarrier && state.cmdCopyImageToBuffer &&
           state.queueSubmit && state.createBuffer && state.destroyBuffer &&
           state.getBufferMemoryRequirements && state.allocateMemory && state.freeMemory &&
           state.bindBufferMemory && state.mapMemory && state.unmapMemory &&
           gVulkanDispatch.getPhysicalDeviceMemoryProperties != nullptr;
}

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice,
                        uint32_t typeFilter,
                        VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    gVulkanDispatch.getPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

void DestroyStagingBuffer(VulkanEncoderState& state)
{
    if (state.stagingBuffer != VK_NULL_HANDLE && state.destroyBuffer != nullptr)
    {
        state.destroyBuffer(state.context.device, state.stagingBuffer, nullptr);
    }
    if (state.stagingMemory != VK_NULL_HANDLE && state.freeMemory != nullptr)
    {
        state.freeMemory(state.context.device, state.stagingMemory, nullptr);
    }
    state.stagingBuffer = VK_NULL_HANDLE;
    state.stagingMemory = VK_NULL_HANDLE;
    state.stagingSize = 0;
}

bool EnsureStagingBuffer(VulkanEncoderState& state, size_t size)
{
    if (state.stagingBuffer != VK_NULL_HANDLE && state.stagingSize >= size)
    {
        return true;
    }

    DestroyStagingBuffer(state);

    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (state.createBuffer(state.context.device, &bufferInfo, nullptr, &state.stagingBuffer) != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements requirements = {};
    state.getBufferMemoryRequirements(state.context.device, state.stagingBuffer, &requirements);
    const uint32_t memoryType = FindMemoryType(
        state.context.physicalDevice,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == UINT32_MAX)
    {
        DestroyStagingBuffer(state);
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = memoryType;
    if (state.allocateMemory(state.context.device, &allocInfo, nullptr, &state.stagingMemory) != VK_SUCCESS)
    {
        DestroyStagingBuffer(state);
        return false;
    }
    if (state.bindBufferMemory(state.context.device, state.stagingBuffer, state.stagingMemory, 0) != VK_SUCCESS)
    {
        DestroyStagingBuffer(state);
        return false;
    }

    state.stagingSize = size;
    return true;
}

bool InitializeVulkanState(VulkanEncoderState& state)
{
    if (state.context.device == VK_NULL_HANDLE ||
        state.context.physicalDevice == VK_NULL_HANDLE ||
        state.context.queue == VK_NULL_HANDLE)
    {
        return false;
    }
    if (!LoadVulkanEncoderFunctions(state))
    {
        return false;
    }

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = state.context.queueFamilyIndex;
    if (state.createCommandPool(state.context.device, &poolInfo, nullptr, &state.commandPool) != VK_SUCCESS)
    {
        return false;
    }

    VkCommandBufferAllocateInfo commandInfo = {};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = state.commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    if (state.allocateCommandBuffers(state.context.device, &commandInfo, &state.commandBuffer) != VK_SUCCESS)
    {
        return false;
    }

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (state.createFence(state.context.device, &fenceInfo, nullptr, &state.fence) != VK_SUCCESS)
    {
        return false;
    }

    return true;
}

void DestroyVulkanState(VulkanEncoderState* state)
{
    if (state == nullptr)
    {
        return;
    }

#if defined(_WIN32)
    ReleaseD3D11StagingTexture(*state);
    ReleaseD3D12ReadbackBuffer(*state);
    if (state->d3d12FenceEvent != nullptr)
    {
        CloseHandle(state->d3d12FenceEvent);
        state->d3d12FenceEvent = nullptr;
    }
    if (state->d3d12Fence != nullptr)
    {
        state->d3d12Fence->Release();
        state->d3d12Fence = nullptr;
    }
    if (state->d3d12CommandList != nullptr)
    {
        state->d3d12CommandList->Release();
        state->d3d12CommandList = nullptr;
    }
    if (state->d3d12CommandAllocator != nullptr)
    {
        state->d3d12CommandAllocator->Release();
        state->d3d12CommandAllocator = nullptr;
    }
#endif

    DestroyStagingBuffer(*state);
    if (state->eyeScaleContext != nullptr)
    {
        sws_freeContext(state->eyeScaleContext);
    }
    if (state->rgbaToYuvContext != nullptr)
    {
        sws_freeContext(state->rgbaToYuvContext);
    }
    if (state->fence != VK_NULL_HANDLE && state->destroyFence != nullptr)
    {
        state->destroyFence(state->context.device, state->fence, nullptr);
    }
    if (state->commandPool != VK_NULL_HANDLE && state->destroyCommandPool != nullptr)
    {
        state->destroyCommandPool(state->context.device, state->commandPool, nullptr);
    }
    delete state;
}

bool ReadVulkanFrameSource(VulkanEncoderState& state,
                           const Swapchain::VulkanFrameSource& source,
                           std::vector<uint8_t>& output)
{
    const auto format = AvPixelFormatForVulkanFormat(source.format);
    if (!format.has_value() || source.image == VK_NULL_HANDLE ||
        source.width == 0 || source.height == 0)
    {
        return false;
    }

    const size_t bytesPerPixel = 4;
    const size_t readbackSize = static_cast<size_t>(source.width) * source.height * bytesPerPixel;
    if (!EnsureStagingBuffer(state, readbackSize))
    {
        return false;
    }

    state.resetFences(state.context.device, 1, &state.fence);
    state.resetCommandPool(state.context.device, state.commandPool, 0);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (state.beginCommandBuffer(state.commandBuffer, &beginInfo) != VK_SUCCESS)
    {
        return false;
    }

    VkImageMemoryBarrier toTransfer = {};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = source.image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = 0;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.baseArrayLayer = source.arrayLayer;
    toTransfer.subresourceRange.layerCount = 1;

    state.cmdPipelineBarrier(
        state.commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = source.arrayLayer;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {source.width, source.height, 1};
    state.cmdCopyImageToBuffer(state.commandBuffer, source.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               state.stagingBuffer, 1, &region);

    VkImageMemoryBarrier backToColor = toTransfer;
    backToColor.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    backToColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    backToColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    backToColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    state.cmdPipelineBarrier(
        state.commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &backToColor);

    if (state.endCommandBuffer(state.commandBuffer) != VK_SUCCESS)
    {
        return false;
    }

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &state.commandBuffer;
    if (state.queueSubmit(state.context.queue, 1, &submitInfo, state.fence) != VK_SUCCESS)
    {
        return false;
    }
    if (state.waitForFences(state.context.device, 1, &state.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
    {
        return false;
    }

    void* mapped = nullptr;
    if (state.mapMemory(state.context.device, state.stagingMemory, 0, readbackSize, 0, &mapped) != VK_SUCCESS)
    {
        return false;
    }
    output.resize(readbackSize);
    std::memcpy(output.data(), mapped, readbackSize);
    state.unmapMemory(state.context.device, state.stagingMemory);
    return true;
}

#if defined(_WIN32)
bool ReadD3D11FrameSource(VulkanEncoderState& state,
                          const Swapchain::D3D11FrameSource& source,
                          std::vector<uint8_t>& output)
{
    const auto format = AvPixelFormatForDxgiFormat(source.format);
    if (!format.has_value() || source.texture == nullptr ||
        state.d3d11Context.immediateContext == nullptr ||
        source.width == 0 || source.height == 0)
    {
        return false;
    }
    if (!EnsureD3D11StagingTexture(state, source))
    {
        return false;
    }

    const UINT sourceSubresource = D3D11CalcSubresource(
        0, source.arrayLayer, 1);
    state.d3d11Context.immediateContext->CopySubresourceRegion(
        state.d3d11StagingTexture,
        0,
        0,
        0,
        0,
        source.texture,
        sourceSubresource,
        nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT result = state.d3d11Context.immediateContext->Map(
        state.d3d11StagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        return false;
    }

    const size_t rowBytes = static_cast<size_t>(source.width) * 4;
    output.resize(rowBytes * source.height);
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t y = 0; y < source.height; ++y)
    {
        std::memcpy(output.data() + rowBytes * y,
                    src + static_cast<size_t>(mapped.RowPitch) * y,
                    rowBytes);
    }
    state.d3d11Context.immediateContext->Unmap(state.d3d11StagingTexture, 0);
    return true;
}

bool ReadD3D12FrameSource(VulkanEncoderState& state,
                          const Swapchain::D3D12FrameSource& source,
                          std::vector<uint8_t>& output)
{
    const auto format = AvPixelFormatForDxgiFormat(source.format);
    if (!format.has_value() || source.texture == nullptr ||
        state.d3d12Context.device == nullptr || state.d3d12Context.queue == nullptr ||
        state.d3d12CommandAllocator == nullptr || state.d3d12CommandList == nullptr ||
        state.d3d12Fence == nullptr || source.width == 0 || source.height == 0)
    {
        return false;
    }

    D3D12_RESOURCE_DESC textureDesc = source.texture->GetDesc();
    if (source.arrayLayer >= textureDesc.DepthOrArraySize)
    {
        return false;
    }
    const UINT sourceSubresource = source.arrayLayer * textureDesc.MipLevels;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    state.d3d12Context.device->GetCopyableFootprints(
        &textureDesc,
        sourceSubresource,
        1,
        0,
        &footprint,
        &numRows,
        &rowSizeInBytes,
        &totalBytes);
    if (totalBytes == 0 || rowSizeInBytes < static_cast<UINT64>(source.width) * 4)
    {
        return false;
    }
    if (!EnsureD3D12ReadbackBuffer(state, static_cast<size_t>(totalBytes)))
    {
        return false;
    }

    if (FAILED(state.d3d12CommandAllocator->Reset()))
    {
        return false;
    }
    if (FAILED(state.d3d12CommandList->Reset(state.d3d12CommandAllocator, nullptr)))
    {
        return false;
    }

    D3D12_RESOURCE_BARRIER toCopy = {};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = source.texture;
    toCopy.Transition.Subresource = sourceSubresource;
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    state.d3d12CommandList->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = state.d3d12ReadbackBuffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = source.texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = sourceSubresource;
    state.d3d12CommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER backToRender = toCopy;
    backToRender.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    backToRender.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    state.d3d12CommandList->ResourceBarrier(1, &backToRender);

    if (FAILED(state.d3d12CommandList->Close()))
    {
        return false;
    }
    ID3D12CommandList* lists[] = {state.d3d12CommandList};
    state.d3d12Context.queue->ExecuteCommandLists(1, lists);

    const uint64_t fenceValue = ++state.d3d12FenceValue;
    if (FAILED(state.d3d12Context.queue->Signal(state.d3d12Fence, fenceValue)))
    {
        return false;
    }
    if (state.d3d12Fence->GetCompletedValue() < fenceValue)
    {
        if (FAILED(state.d3d12Fence->SetEventOnCompletion(fenceValue, state.d3d12FenceEvent)))
        {
            return false;
        }
        WaitForSingleObject(state.d3d12FenceEvent, INFINITE);
    }

    D3D12_RANGE readRange = {
        static_cast<SIZE_T>(footprint.Offset),
        static_cast<SIZE_T>(footprint.Offset + totalBytes),
    };
    void* mapped = nullptr;
    if (FAILED(state.d3d12ReadbackBuffer->Map(0, &readRange, &mapped)))
    {
        return false;
    }

    const size_t rowBytes = static_cast<size_t>(source.width) * 4;
    output.resize(rowBytes * source.height);
    const auto* mappedBytes = static_cast<const uint8_t*>(mapped);
    const auto* srcRows = mappedBytes + footprint.Offset;
    for (uint32_t y = 0; y < source.height; ++y)
    {
        std::memcpy(output.data() + rowBytes * y,
                    srcRows + static_cast<size_t>(footprint.Footprint.RowPitch) * y,
                    rowBytes);
    }
    D3D12_RANGE writtenRange = {0, 0};
    state.d3d12ReadbackBuffer->Unmap(0, &writtenRange);
    return true;
}
#endif

bool ScaleEyeToStereoRgba(VulkanEncoderState& state,
                          AVPixelFormat sourceFormat,
                          uint32_t sourceWidth,
                          uint32_t sourceHeight,
                          const std::vector<uint8_t>& readback,
                          uint32_t sourceX,
                          uint32_t sourceY,
                          uint32_t sourceCropWidth,
                          uint32_t sourceCropHeight,
                          uint32_t outputX,
                          uint32_t outputEyeWidth,
                          uint32_t outputHeight,
                          uint32_t outputStereoWidth)
{
    state.stereoRgba.resize(static_cast<size_t>(outputStereoWidth) * outputHeight * 4);
    uint8_t* dstData[4] = {
        state.stereoRgba.data() + static_cast<size_t>(outputX) * 4,
        nullptr, nullptr, nullptr,
    };
    int dstLinesize[4] = {
        static_cast<int>(outputStereoWidth * 4),
        0, 0, 0,
    };
    const size_t sourceOffset =
        (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4;
    const uint8_t* srcData[4] = {readback.data() + sourceOffset, nullptr, nullptr, nullptr};
    int srcLinesize[4] = {static_cast<int>(sourceWidth * 4), 0, 0, 0};

    state.eyeScaleContext = sws_getCachedContext(
        state.eyeScaleContext,
        static_cast<int>(sourceCropWidth),
        static_cast<int>(sourceCropHeight),
        sourceFormat,
        static_cast<int>(outputEyeWidth),
        static_cast<int>(outputHeight),
        AV_PIX_FMT_RGBA,
        SWS_FAST_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (state.eyeScaleContext == nullptr)
    {
        return false;
    }

    return sws_scale(state.eyeScaleContext,
                     srcData,
                     srcLinesize,
                     0,
                     static_cast<int>(sourceCropHeight),
                     dstData,
                     dstLinesize) > 0;
}

struct SourceRect
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

std::optional<SourceRect> ResolveSourceRect(const FrameImageSource& source,
                                            uint32_t imageWidth,
                                            uint32_t imageHeight)
{
    SourceRect rect = {};
    if (source.HasSourceRect())
    {
        rect.x = source.sourceX;
        rect.y = source.sourceY;
        rect.width = source.sourceWidth;
        rect.height = source.sourceHeight;
    }
    else
    {
        rect.width = imageWidth;
        rect.height = imageHeight;
    }

    const uint64_t maxX = static_cast<uint64_t>(rect.x) + rect.width;
    const uint64_t maxY = static_cast<uint64_t>(rect.y) + rect.height;
    if (rect.width == 0 || rect.height == 0 ||
        maxX > imageWidth || maxY > imageHeight)
    {
        return std::nullopt;
    }
    return rect;
}

bool ConvertStereoRgbaToYuv(VulkanEncoderState& state,
                            AVFrame* frame,
                            uint32_t width,
                            uint32_t height)
{
    state.rgbaToYuvContext = sws_getCachedContext(
        state.rgbaToYuvContext,
        static_cast<int>(width),
        static_cast<int>(height),
        AV_PIX_FMT_RGBA,
        frame->width,
        frame->height,
        static_cast<AVPixelFormat>(frame->format),
        SWS_FAST_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (state.rgbaToYuvContext == nullptr)
    {
        return false;
    }

    const uint8_t* srcData[4] = {state.stereoRgba.data(), nullptr, nullptr, nullptr};
    int srcLinesize[4] = {static_cast<int>(width * 4), 0, 0, 0};
    return sws_scale(state.rgbaToYuvContext,
                     srcData,
                     srcLinesize,
                     0,
                     static_cast<int>(height),
                     frame->data,
                     frame->linesize) > 0;
}

} // namespace

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder()
{
    Shutdown();
}

bool VideoEncoder::SupportsFoveatedEncoding(const GraphicsContext& /*graphicsContext*/)
{
    return false;
}

bool VideoEncoder::Initialize(uint32_t width, uint32_t height, uint32_t fps,
                              uint32_t bitrateMbps, const GraphicsContext& graphicsContext)
{
    Shutdown();

    width_ = width;
    height_ = height;
    eyeWidth_ = width / 2;
    fps_ = std::max(fps, 1u);
    bitrateMbps_ = bitrateMbps;
    graphicsContext_ = graphicsContext;
    frameCount_ = 0;
    forceKeyframe_.store(false);
    shuttingDown_.store(false);
    droppedFrameCount_.store(0);
    inFlightFrameCount_.store(0);
    frameNumberCounter_.store(0);

    const GraphicsApi graphicsApi = graphicsContext_.api;
    std::unique_ptr<VulkanEncoderState, decltype(&DestroyVulkanState)> vkState(
        new VulkanEncoderState(), DestroyVulkanState);
    vkState->graphicsApi = graphicsApi;
    if (graphicsApi == GraphicsApi::Vulkan)
    {
        vkState->context = graphicsContext_.vulkan;
        if (!InitializeVulkanState(*vkState))
        {
            spdlog::error("FFmpegVideoEncoder: failed to initialize Vulkan readback state");
            return false;
        }
    }
#if defined(_WIN32)
    else if (graphicsApi == GraphicsApi::D3D11)
    {
        if (graphicsContext_.d3d11.device == nullptr ||
            graphicsContext_.d3d11.immediateContext == nullptr)
        {
            spdlog::error("FFmpegVideoEncoder: missing D3D11 graphics context");
            return false;
        }
        vkState->d3d11Context = graphicsContext_.d3d11;
    }
    else if (graphicsApi == GraphicsApi::D3D12)
    {
        if (graphicsContext_.d3d12.device == nullptr ||
            graphicsContext_.d3d12.queue == nullptr)
        {
            spdlog::error("FFmpegVideoEncoder: missing D3D12 graphics context");
            return false;
        }
        vkState->d3d12Context = graphicsContext_.d3d12;
        if (!InitializeD3D12State(*vkState))
        {
            spdlog::error("FFmpegVideoEncoder: failed to initialize D3D12 readback state");
            return false;
        }
    }
#endif
    else
    {
        spdlog::error("FFmpegVideoEncoder: unsupported graphics backend for FFmpeg encoder");
        return false;
    }

    const AVCodec* codec = SelectHevcEncoder();
    if (codec == nullptr)
    {
        spdlog::error("FFmpegVideoEncoder: no HEVC encoder available");
        return false;
    }

    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == nullptr)
    {
        spdlog::error("FFmpegVideoEncoder: failed to allocate codec context");
        return false;
    }

    context->width = static_cast<int>(width_);
    context->height = static_cast<int>(height_);
    context->time_base = AVRational{1, static_cast<int>(fps_)};
    context->framerate = AVRational{static_cast<int>(fps_), 1};
    context->pix_fmt = ChooseEncoderPixelFormat(codec);
    context->bit_rate = static_cast<int64_t>(bitrateMbps_) * 1000 * 1000;
    context->rc_max_rate = context->bit_rate;
    context->rc_buffer_size = static_cast<int>(std::max<int64_t>(context->bit_rate / std::max<uint32_t>(fps_, 1), 1));
    context->gop_size = static_cast<int>(std::max(Config::Get().GetValues().keyframeIntervalSec * fps_, 1u));
    context->max_b_frames = 0;
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;

    const std::string encoderPreset = Config::Get().GetValues().encoderPreset;
    const char* ffmpegPreset = "superfast";
    if (encoderPreset == "speed")
    {
        ffmpegPreset = "ultrafast";
    }
    else if (encoderPreset == "quality")
    {
        ffmpegPreset = "veryfast";
    }

    if (IsMediaFoundationEncoder(codec))
    {
        av_opt_set(context->priv_data, "rate_control", "cbr", 0);
        av_opt_set(context->priv_data, "scenario", "camera_record", 0);
        av_opt_set_int(context->priv_data, "hw_encoding", 1, 0);
    }
    else
    {
        av_opt_set(context->priv_data, "preset", ffmpegPreset, 0);
        av_opt_set(context->priv_data, "tune", "zerolatency", 0);
    }

    if (avcodec_open2(context, codec, nullptr) < 0)
    {
        spdlog::error("FFmpegVideoEncoder: failed to open HEVC encoder {} with pixel format {}",
                      codec->name,
                      av_get_pix_fmt_name(context->pix_fmt));
        avcodec_free_context(&context);
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    if (frame == nullptr || packet == nullptr)
    {
        spdlog::error("FFmpegVideoEncoder: failed to allocate frame or packet");
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
        return false;
    }

    frame->format = context->pix_fmt;
    frame->width = context->width;
    frame->height = context->height;
    frame->duration = 1;
    if (av_frame_get_buffer(frame, 32) < 0)
    {
        spdlog::error("FFmpegVideoEncoder: failed to allocate frame buffer");
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
        return false;
    }

    ffmpeg_.codecContext = context;
    ffmpeg_.frame = frame;
    ffmpeg_.packet = packet;
    ffmpeg_.readbackState = vkState.release();

    const char* backendName =
        graphicsApi == GraphicsApi::Vulkan ? "Vulkan" :
        graphicsApi == GraphicsApi::D3D11 ? "D3D11" :
        graphicsApi == GraphicsApi::D3D12 ? "D3D12" : "unknown";
    spdlog::info("FFmpegVideoEncoder: initialized {} HEVC encoder {} {}x{} @ {}Hz {}Mbps pix_fmt={} preset={} ({})",
                 backendName,
                 codec->name,
                 width_,
                 height_,
                 fps_,
                 bitrateMbps_,
                 av_get_pix_fmt_name(context->pix_fmt),
                 encoderPreset,
                 IsMediaFoundationEncoder(codec) ? "mediafoundation-cbr" : ffmpegPreset);
    return true;
}

void VideoEncoder::Shutdown()
{
    shuttingDown_.store(true);

    AVCodecContext* context = CodecContext(ffmpeg_.codecContext);
    AVFrame* frame = Frame(ffmpeg_.frame);
    AVPacket* packet = Packet(ffmpeg_.packet);
    VulkanEncoderState* vkState = State(ffmpeg_.readbackState);

    if (context != nullptr)
    {
        avcodec_free_context(&context);
    }
    if (frame != nullptr)
    {
        av_frame_free(&frame);
    }
    if (packet != nullptr)
    {
        av_packet_free(&packet);
    }
    DestroyVulkanState(vkState);

    ffmpeg_.codecContext = nullptr;
    ffmpeg_.frame = nullptr;
    ffmpeg_.packet = nullptr;
    ffmpeg_.readbackState = nullptr;
    inFlightFrameCount_.store(0);
    consecutiveNoPacketFrames_.store(0);
}

bool VideoEncoder::Encode(FrameImageSource imageSource, int64_t timestampNs, OnNalUnitCallback callback,
                          OnFrameEncodedCallback frameCallback)
{
    FrameSource frameSource = {};
    frameSource.left = std::move(imageSource);
    return EncodeInternal(std::move(frameSource), false, timestampNs,
                          std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeStereo(FrameSource frameSource, int64_t timestampNs, OnNalUnitCallback callback,
                                OnFrameEncodedCallback frameCallback)
{
    return EncodeInternal(std::move(frameSource), true, timestampNs,
                          std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeInternal(FrameSource frameSource, bool stereo,
                                  int64_t timestampNs, OnNalUnitCallback callback,
                                  OnFrameEncodedCallback frameCallback)
{
    AVCodecContext* context = CodecContext(ffmpeg_.codecContext);
    AVFrame* frame = Frame(ffmpeg_.frame);
    AVPacket* packet = Packet(ffmpeg_.packet);
    VulkanEncoderState* vkState = State(ffmpeg_.readbackState);
    if (context == nullptr || frame == nullptr || packet == nullptr || vkState == nullptr)
    {
        return false;
    }

    auto encodeStart = Clock::now();
    inFlightFrameCount_.fetch_add(1);

    VideoEncoder::FrameMetrics metrics = {};
    metrics.frameNumber = frameNumberCounter_.fetch_add(1) + 1;
    metrics.timestampNs = timestampNs;

    auto dropFrame = [&]() {
        droppedFrameCount_.fetch_add(1);
        inFlightFrameCount_.fetch_sub(1);
        metrics.frameDropped = true;
        metrics.totalLatencyMs = ToMilliseconds(Clock::now() - encodeStart);
        if (frameCallback)
        {
            frameCallback(metrics);
        }
        return false;
    };

    if (av_frame_make_writable(frame) < 0)
    {
        return dropFrame();
    }

    auto copyStart = Clock::now();
    std::vector<uint8_t> leftReadback;
    std::vector<uint8_t> rightReadback;

    if (graphicsContext_.api == GraphicsApi::Vulkan)
    {
        auto* leftSource =
            static_cast<Swapchain::VulkanFrameSource*>(frameSource.left.GetImage());
        auto* rightSource = static_cast<Swapchain::VulkanFrameSource*>(
            (stereo ? frameSource.right : frameSource.left).GetImage());
        if (leftSource == nullptr || rightSource == nullptr)
        {
            spdlog::warn("FFmpegVideoEncoder: missing Vulkan frame source");
            return dropFrame();
        }

        const auto leftFormat = AvPixelFormatForVulkanFormat(leftSource->format);
        const auto rightFormat = AvPixelFormatForVulkanFormat(rightSource->format);
        const uint32_t leftWidth = leftSource->width;
        const uint32_t leftHeight = leftSource->height;
        const uint32_t rightWidth = rightSource->width;
        const uint32_t rightHeight = rightSource->height;
        const std::optional<SourceRect> leftRect =
            ResolveSourceRect(frameSource.left, leftWidth, leftHeight);
        const std::optional<SourceRect> rightRect =
            ResolveSourceRect(stereo ? frameSource.right : frameSource.left, rightWidth, rightHeight);
        if (!leftFormat.has_value() || !rightFormat.has_value() ||
            !leftRect.has_value() || !rightRect.has_value() ||
            !ReadVulkanFrameSource(*vkState, *leftSource, leftReadback) ||
            !ReadVulkanFrameSource(*vkState, *rightSource, rightReadback))
        {
            spdlog::warn("FFmpegVideoEncoder: Vulkan readback failed or unsupported format");
            return dropFrame();
        }
        if (metrics.frameNumber == 1)
        {
            spdlog::info("FFmpegVideoEncoder: Vulkan source rect L={}x{}+{},{} R={}x{}+{},{}",
                         leftRect->width, leftRect->height, leftRect->x, leftRect->y,
                         rightRect->width, rightRect->height, rightRect->x, rightRect->y);
        }
        frameSource = {};
        if (!ScaleEyeToStereoRgba(*vkState, *leftFormat, leftWidth, leftHeight,
                                  leftReadback, leftRect->x, leftRect->y,
                                  leftRect->width, leftRect->height,
                                  0, eyeWidth_, height_, width_) ||
            !ScaleEyeToStereoRgba(*vkState, *rightFormat, rightWidth, rightHeight,
                                  rightReadback, rightRect->x, rightRect->y,
                                  rightRect->width, rightRect->height,
                                  eyeWidth_, eyeWidth_, height_, width_) ||
            !ConvertStereoRgbaToYuv(*vkState, frame, width_, height_))
        {
            spdlog::warn("FFmpegVideoEncoder: Vulkan frame conversion failed");
            return dropFrame();
        }
    }
#if defined(_WIN32)
    else if (graphicsContext_.api == GraphicsApi::D3D11)
    {
        auto* leftSource =
            static_cast<Swapchain::D3D11FrameSource*>(frameSource.left.GetImage());
        auto* rightSource = static_cast<Swapchain::D3D11FrameSource*>(
            (stereo ? frameSource.right : frameSource.left).GetImage());
        if (leftSource == nullptr || rightSource == nullptr)
        {
            spdlog::warn("FFmpegVideoEncoder: missing D3D11 frame source");
            return dropFrame();
        }

        const auto leftFormat = AvPixelFormatForDxgiFormat(leftSource->format);
        const auto rightFormat = AvPixelFormatForDxgiFormat(rightSource->format);
        const uint32_t leftWidth = leftSource->width;
        const uint32_t leftHeight = leftSource->height;
        const uint32_t rightWidth = rightSource->width;
        const uint32_t rightHeight = rightSource->height;
        const std::optional<SourceRect> leftRect =
            ResolveSourceRect(frameSource.left, leftWidth, leftHeight);
        const std::optional<SourceRect> rightRect =
            ResolveSourceRect(stereo ? frameSource.right : frameSource.left, rightWidth, rightHeight);
        if (!leftFormat.has_value() || !rightFormat.has_value() ||
            !leftRect.has_value() || !rightRect.has_value() ||
            !ReadD3D11FrameSource(*vkState, *leftSource, leftReadback) ||
            !ReadD3D11FrameSource(*vkState, *rightSource, rightReadback))
        {
            spdlog::warn("FFmpegVideoEncoder: D3D11 readback failed or unsupported format");
            return dropFrame();
        }
        if (metrics.frameNumber == 1)
        {
            spdlog::info("FFmpegVideoEncoder: D3D11 source rect L={}x{}+{},{} R={}x{}+{},{}",
                         leftRect->width, leftRect->height, leftRect->x, leftRect->y,
                         rightRect->width, rightRect->height, rightRect->x, rightRect->y);
        }
        frameSource = {};
        if (!ScaleEyeToStereoRgba(*vkState, *leftFormat, leftWidth, leftHeight,
                                  leftReadback, leftRect->x, leftRect->y,
                                  leftRect->width, leftRect->height,
                                  0, eyeWidth_, height_, width_) ||
            !ScaleEyeToStereoRgba(*vkState, *rightFormat, rightWidth, rightHeight,
                                  rightReadback, rightRect->x, rightRect->y,
                                  rightRect->width, rightRect->height,
                                  eyeWidth_, eyeWidth_, height_, width_) ||
            !ConvertStereoRgbaToYuv(*vkState, frame, width_, height_))
        {
            spdlog::warn("FFmpegVideoEncoder: D3D11 frame conversion failed");
            return dropFrame();
        }
    }
    else if (graphicsContext_.api == GraphicsApi::D3D12)
    {
        auto* leftSource =
            static_cast<Swapchain::D3D12FrameSource*>(frameSource.left.GetImage());
        auto* rightSource = static_cast<Swapchain::D3D12FrameSource*>(
            (stereo ? frameSource.right : frameSource.left).GetImage());
        if (leftSource == nullptr || rightSource == nullptr)
        {
            spdlog::warn("FFmpegVideoEncoder: missing D3D12 frame source");
            return dropFrame();
        }

        const auto leftFormat = AvPixelFormatForDxgiFormat(leftSource->format);
        const auto rightFormat = AvPixelFormatForDxgiFormat(rightSource->format);
        const uint32_t leftWidth = leftSource->width;
        const uint32_t leftHeight = leftSource->height;
        const uint32_t rightWidth = rightSource->width;
        const uint32_t rightHeight = rightSource->height;
        const std::optional<SourceRect> leftRect =
            ResolveSourceRect(frameSource.left, leftWidth, leftHeight);
        const std::optional<SourceRect> rightRect =
            ResolveSourceRect(stereo ? frameSource.right : frameSource.left, rightWidth, rightHeight);
        if (!leftFormat.has_value() || !rightFormat.has_value() ||
            !leftRect.has_value() || !rightRect.has_value() ||
            !ReadD3D12FrameSource(*vkState, *leftSource, leftReadback) ||
            !ReadD3D12FrameSource(*vkState, *rightSource, rightReadback))
        {
            spdlog::warn("FFmpegVideoEncoder: D3D12 readback failed or unsupported format");
            return dropFrame();
        }
        if (metrics.frameNumber == 1)
        {
            spdlog::info("FFmpegVideoEncoder: D3D12 source rect L={}x{}+{},{} R={}x{}+{},{}",
                         leftRect->width, leftRect->height, leftRect->x, leftRect->y,
                         rightRect->width, rightRect->height, rightRect->x, rightRect->y);
        }
        frameSource = {};
        if (!ScaleEyeToStereoRgba(*vkState, *leftFormat, leftWidth, leftHeight,
                                  leftReadback, leftRect->x, leftRect->y,
                                  leftRect->width, leftRect->height,
                                  0, eyeWidth_, height_, width_) ||
            !ScaleEyeToStereoRgba(*vkState, *rightFormat, rightWidth, rightHeight,
                                  rightReadback, rightRect->x, rightRect->y,
                                  rightRect->width, rightRect->height,
                                  eyeWidth_, eyeWidth_, height_, width_) ||
            !ConvertStereoRgbaToYuv(*vkState, frame, width_, height_))
        {
            spdlog::warn("FFmpegVideoEncoder: D3D12 frame conversion failed");
            return dropFrame();
        }
    }
#endif
    else
    {
        return dropFrame();
    }
    metrics.gpuCopyMs = ToMilliseconds(Clock::now() - copyStart);

    frame->pts = static_cast<int64_t>(frameCount_);
    frame->duration = 1;
    if (forceKeyframe_.exchange(false))
    {
        frame->pict_type = AV_PICTURE_TYPE_I;
        metrics.keyframe = true;
    }
    else
    {
        frame->pict_type = AV_PICTURE_TYPE_NONE;
    }

    auto submitStart = Clock::now();
    int sendResult = avcodec_send_frame(context, frame);
    metrics.encodeSubmitMs = ToMilliseconds(Clock::now() - submitStart);
    if (sendResult < 0)
    {
        spdlog::warn("FFmpegVideoEncoder: avcodec_send_frame failed: {}",
                     FfmpegErrorString(sendResult));
        return dropFrame();
    }

    bool emittedPacket = false;
    for (;;)
    {
        int receiveResult = avcodec_receive_packet(context, packet);
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF)
        {
            break;
        }
        if (receiveResult < 0)
        {
            spdlog::warn("FFmpegVideoEncoder: avcodec_receive_packet failed: {}",
                         FfmpegErrorString(receiveResult));
            droppedFrameCount_.fetch_add(1);
            metrics.frameDropped = true;
            break;
        }

        bool packetIsKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        metrics.keyframe = metrics.keyframe || packetIsKeyframe;
        if (callback && packet->data != nullptr && packet->size > 0)
        {
            callback(packet->data, static_cast<size_t>(packet->size), packetIsKeyframe, timestampNs);
        }
        emittedPacket = true;
        consecutiveNoPacketFrames_.store(0);
        av_packet_unref(packet);
    }

    frameCount_++;
    metrics.totalLatencyMs = ToMilliseconds(Clock::now() - encodeStart);
    inFlightFrameCount_.fetch_sub(1);

    if (!emittedPacket)
    {
        const uint32_t noPacketFrames = consecutiveNoPacketFrames_.fetch_add(1) + 1;
        if (noPacketFrames == 5 || noPacketFrames == 30 || noPacketFrames % 120 == 0)
        {
            spdlog::warn("FFmpegVideoEncoder: encoder accepted {} consecutive frame(s) without output packets",
                         noPacketFrames);
        }
        droppedFrameCount_.fetch_add(1);
        metrics.frameDropped = true;
    }

    if (frameCallback)
    {
        frameCallback(metrics);
    }

    return emittedPacket;
}

void VideoEncoder::ForceKeyframe()
{
    forceKeyframe_.store(true);
}

void VideoEncoder::SetBitrate(uint32_t bitrateMbps)
{
    bitrateMbps_ = bitrateMbps;
    if (AVCodecContext* context = CodecContext(ffmpeg_.codecContext))
    {
        context->bit_rate = static_cast<int64_t>(bitrateMbps_) * 1000 * 1000;
    }
}

bool VideoEncoder::AcquireSlot(size_t& outSlotIndex)
{
    outSlotIndex = 0;
    return false;
}

void VideoEncoder::ReleaseSlot(size_t /*slotIndex*/)
{
}

void VideoEncoder::DestroySlots()
{
}
