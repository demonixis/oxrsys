// SPDX-License-Identifier: MPL-2.0

#include "Swapchain.h"
#include "Runtime.h"

#if defined(_WIN32) && (defined(OXRSYS_USE_D3D12) || defined(XR_USE_GRAPHICS_API_D3D12))

#include <spdlog/spdlog.h>

#include <iomanip>
#include <sstream>
#include <thread>

namespace
{

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

std::string HResultString(HRESULT result)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    return stream.str();
}

bool IsD3DDepthFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_D16_UNORM ||
           format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
           format == DXGI_FORMAT_D32_FLOAT ||
           format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

bool IsSupportedD3DFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            return false;
    }
}

D3D12_RESOURCE_STATES RenderStateForD3D12Format(DXGI_FORMAT format)
{
    return IsD3DDepthFormat(format)
        ? D3D12_RESOURCE_STATE_DEPTH_WRITE
        : D3D12_RESOURCE_STATE_RENDER_TARGET;
}

} // namespace

void Swapchain::InitD3D12(const D3D12GraphicsContext& d3d12Context,
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
    graphicsApi_ = GraphicsApi::D3D12;
    d3d12Context_ = d3d12Context;

    if (d3d12Context_.device == nullptr || d3d12Context_.queue == nullptr)
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: missing D3D12 graphics context for swapchain creation");
        return;
    }

    const DXGI_FORMAT dxgiFormat = static_cast<DXGI_FORMAT>(format_);
    if (!IsSupportedD3DFormat(dxgiFormat))
    {
        initializationResult_ = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: unsupported D3D12 swapchain format {}", format_);
        return;
    }

    d3d12Resources_.resize(imageCount_, nullptr);
    textures_.resize(imageCount_, nullptr);
    imageStates_.assign(imageCount_, ImageState::Available);
    lastD3D12Snapshots_.assign(arraySize_, {});

    const bool isDepth = IsD3DDepthFormat(dxgiFormat);
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width_;
    desc.Height = height_;
    desc.DepthOrArraySize = static_cast<UINT16>(arraySize_);
    desc.MipLevels = 1;
    desc.Format = dxgiFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                         : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = dxgiFormat;
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 1.0f;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    for (uint32_t i = 0; i < imageCount_; ++i)
    {
        ID3D12Resource* resource = nullptr;
        HRESULT result = d3d12Context_.device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            RenderStateForD3D12Format(dxgiFormat),
            isDepth ? &clearValue : nullptr,
            __uuidof(ID3D12Resource),
            reinterpret_cast<void**>(&resource));
        if (FAILED(result))
        {
            spdlog::error("OXRSys: ID3D12Device::CreateCommittedResource failed with {}",
                          HResultString(result));
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            continue;
        }
        d3d12Resources_[i] = resource;
    }

    Runtime::Get().RegisterHandle(handle_, this);
    spdlog::info("OXRSys: D3D12 swapchain created {}x{} format={} arraySize={} images={}",
                 width_, height_, format_, arraySize_, imageCount_);
}

XrResult Swapchain::EnumerateD3D12Images(uint32_t /*imageCapacityInput*/,
                                         XrSwapchainImageBaseHeader* images) const
{
    auto* d3dImages = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
    for (uint32_t i = 0; i < imageCount_; i++)
    {
        d3dImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        d3dImages[i].next = nullptr;
        d3dImages[i].texture = i < d3d12Resources_.size()
            ? static_cast<ID3D12Resource*>(d3d12Resources_[i])
            : nullptr;
    }
    return XR_SUCCESS;
}

void Swapchain::DestroyD3D12Resources()
{
    for (void* resource : d3d12Resources_)
    {
        if (resource != nullptr)
        {
            static_cast<ID3D12Resource*>(resource)->Release();
        }
    }
}

FrameImageSource Swapchain::SnapshotD3D12FrameImageSource(uint32_t arrayIndex) const
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Resource* texture = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    std::shared_ptr<SwapchainSnapshotPoolState> pool;
    {
        std::scoped_lock lock(stateMutex_);
        if (!hasReleasedImage_ || arrayIndex >= arraySize_ ||
            lastReleasedIndex_ >= d3d12Resources_.size() ||
            d3d12Context_.device == nullptr || d3d12Context_.queue == nullptr ||
            d3d12Resources_[lastReleasedIndex_] == nullptr)
        {
            return {};
        }
        device = d3d12Context_.device;
        device->AddRef();
        queue = d3d12Context_.queue;
        queue->AddRef();
        texture = static_cast<ID3D12Resource*>(d3d12Resources_[lastReleasedIndex_]);
        texture->AddRef();
        format = static_cast<DXGI_FORMAT>(format_);
        width = width_;
        height = height_;
        pool = backendSnapshotPool_;
    }

    auto releaseLocals = [&]() {
        if (texture != nullptr)
        {
            texture->Release();
            texture = nullptr;
        }
        if (queue != nullptr)
        {
            queue->Release();
            queue = nullptr;
        }
        if (device != nullptr)
        {
            device->Release();
            device = nullptr;
        }
    };

    if (IsD3DDepthFormat(format) || width == 0 || height == 0)
    {
        releaseLocals();
        return {};
    }

    std::shared_ptr<void> lease = TryAcquireBackendSnapshotLease(pool);
    if (!lease)
    {
        releaseLocals();
        spdlog::debug("OXRSys: D3D12 snapshot pool is full; streaming snapshot unavailable");
        return {};
    }

    auto* source = new D3D12FrameSource();
    source->lifetime = lease;
    source->sourceTexture = texture;
    texture = nullptr;
    source->format = format;
    source->width = width;
    source->height = height;

    D3D12_RESOURCE_DESC textureDesc = source->sourceTexture->GetDesc();
    const UINT sourceSubresource = arrayIndex * textureDesc.MipLevels;
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    device->GetCopyableFootprints(
        &textureDesc,
        sourceSubresource,
        1,
        0,
        &source->footprint,
        &numRows,
        &rowSizeInBytes,
        &source->totalBytes);
    if (source->totalBytes == 0 || rowSizeInBytes < static_cast<UINT64>(width) * 4)
    {
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC readbackDesc = {};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = source->totalBytes;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT result = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &readbackDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        __uuidof(ID3D12Resource),
        reinterpret_cast<void**>(&source->readbackBuffer));
    if (FAILED(result))
    {
        spdlog::warn("OXRSys: D3D12 readback buffer creation failed with {}", HResultString(result));
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }

    const D3D12_COMMAND_LIST_TYPE listType = queue->GetDesc().Type;
    if (listType != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        listType != D3D12_COMMAND_LIST_TYPE_COPY)
    {
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }
    if (FAILED(device->CreateCommandAllocator(
            listType,
            __uuidof(ID3D12CommandAllocator),
            reinterpret_cast<void**>(&source->commandAllocator))) ||
        FAILED(device->CreateCommandList(
            0,
            listType,
            source->commandAllocator,
            nullptr,
            __uuidof(ID3D12GraphicsCommandList),
            reinterpret_cast<void**>(&source->commandList))) ||
        FAILED(device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            __uuidof(ID3D12Fence),
            reinterpret_cast<void**>(&source->fence))))
    {
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }
    source->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (source->fenceEvent == nullptr)
    {
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }

    D3D12_RESOURCE_BARRIER toCopy = {};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = source->sourceTexture;
    toCopy.Transition.Subresource = sourceSubresource;
    toCopy.Transition.StateBefore = RenderStateForD3D12Format(format);
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    source->commandList->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = source->readbackBuffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = source->footprint;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = source->sourceTexture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = sourceSubresource;
    source->commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER backToRender = toCopy;
    backToRender.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    backToRender.Transition.StateAfter = RenderStateForD3D12Format(format);
    source->commandList->ResourceBarrier(1, &backToRender);

    if (FAILED(source->commandList->Close()))
    {
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }

    ID3D12CommandList* commandLists[] = {source->commandList};
    queue->ExecuteCommandLists(1, commandLists);
    source->fenceValue = 1;
    if (FAILED(queue->Signal(source->fence, source->fenceValue)))
    {
        releaseLocals();
        ReleaseD3D12FrameSource(source);
        return {};
    }
    source->fenceSubmitted = true;
    releaseLocals();

    FrameImageSource frameSource = {};
    frameSource.api = GraphicsApi::D3D12;
    frameSource.lifetime = lease;
    frameSource.image = std::shared_ptr<void>(source, [](void* value) {
        ReleaseD3D12FrameSource(static_cast<D3D12FrameSource*>(value));
    });
    frameSource.sourceWidth = width;
    frameSource.sourceHeight = height;
    frameSource.sourceFormat = static_cast<uint64_t>(format);
    frameSource.imageWidth = width;
    frameSource.imageHeight = height;
    return frameSource;
}

void Swapchain::ReleaseD3D12FrameSource(D3D12FrameSource* source)
{
    if (source == nullptr)
    {
        return;
    }

    auto destroy = [](D3D12FrameSource* sourceToDestroy) {
        if (sourceToDestroy->fenceSubmitted && sourceToDestroy->fence != nullptr &&
            sourceToDestroy->fence->GetCompletedValue() < sourceToDestroy->fenceValue)
        {
            bool waitingOnEvent = false;
            if (sourceToDestroy->fenceEvent != nullptr &&
                SUCCEEDED(sourceToDestroy->fence->SetEventOnCompletion(
                    sourceToDestroy->fenceValue, sourceToDestroy->fenceEvent)))
            {
                waitingOnEvent = true;
                WaitForSingleObject(sourceToDestroy->fenceEvent, INFINITE);
            }
            while (!waitingOnEvent &&
                   sourceToDestroy->fence->GetCompletedValue() < sourceToDestroy->fenceValue)
            {
                Sleep(1);
            }
        }
        if (sourceToDestroy->commandList != nullptr)
        {
            sourceToDestroy->commandList->Release();
            sourceToDestroy->commandList = nullptr;
        }
        if (sourceToDestroy->commandAllocator != nullptr)
        {
            sourceToDestroy->commandAllocator->Release();
            sourceToDestroy->commandAllocator = nullptr;
        }
        if (sourceToDestroy->readbackBuffer != nullptr)
        {
            sourceToDestroy->readbackBuffer->Release();
            sourceToDestroy->readbackBuffer = nullptr;
        }
        if (sourceToDestroy->sourceTexture != nullptr)
        {
            sourceToDestroy->sourceTexture->Release();
            sourceToDestroy->sourceTexture = nullptr;
        }
        if (sourceToDestroy->fence != nullptr)
        {
            sourceToDestroy->fence->Release();
            sourceToDestroy->fence = nullptr;
        }
        if (sourceToDestroy->fenceEvent != nullptr)
        {
            CloseHandle(sourceToDestroy->fenceEvent);
            sourceToDestroy->fenceEvent = nullptr;
        }
        delete sourceToDestroy;
    };

    if (source->fenceSubmitted && source->fence != nullptr &&
        source->fence->GetCompletedValue() < source->fenceValue)
    {
        std::thread(destroy, source).detach();
        return;
    }
    destroy(source);
}

#endif // defined(_WIN32) && D3D12
