// SPDX-License-Identifier: MPL-2.0

#include "Swapchain.h"
#include "Runtime.h"

#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(XR_USE_GRAPHICS_API_D3D11))

#include "OpenXRPlatform.h"

#include <spdlog/spdlog.h>

#include <iomanip>
#include <sstream>

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

} // namespace

void Swapchain::InitD3D11(const D3D11GraphicsContext& d3d11Context,
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
    graphicsApi_ = GraphicsApi::D3D11;
    d3d11Context_ = d3d11Context;

    if (d3d11Context_.device == nullptr || d3d11Context_.immediateContext == nullptr)
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: missing D3D11 graphics context for swapchain creation");
        return;
    }

    const DXGI_FORMAT dxgiFormat = static_cast<DXGI_FORMAT>(format_);
    if (!IsSupportedD3DFormat(dxgiFormat))
    {
        initializationResult_ = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: unsupported D3D11 swapchain format {}", format_);
        return;
    }

    d3d11Textures_.resize(imageCount_, nullptr);
    textures_.resize(imageCount_, nullptr);
    imageStates_.assign(imageCount_, ImageState::Available);
    lastD3D11Snapshots_.assign(arraySize_, {});

    const bool isDepth = IsD3DDepthFormat(dxgiFormat);
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = arraySize_;
    desc.Format = dxgiFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = isDepth
        ? D3D11_BIND_DEPTH_STENCIL
        : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);

    for (uint32_t i = 0; i < imageCount_; ++i)
    {
        ID3D11Texture2D* texture = nullptr;
        HRESULT result = d3d11Context_.device->CreateTexture2D(&desc, nullptr, &texture);
        if (FAILED(result))
        {
            spdlog::error("OXRSys: ID3D11Device::CreateTexture2D failed with {}",
                          HResultString(result));
            initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
            continue;
        }
        d3d11Textures_[i] = texture;
    }

    Runtime::Get().RegisterHandle(handle_, this);
    spdlog::info("OXRSys: D3D11 swapchain created {}x{} format={} arraySize={} images={}",
                 width_, height_, format_, arraySize_, imageCount_);
}

XrResult Swapchain::EnumerateD3D11Images(uint32_t /*imageCapacityInput*/,
                                         XrSwapchainImageBaseHeader* images) const
{
    auto* d3dImages = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
    for (uint32_t i = 0; i < imageCount_; i++)
    {
        d3dImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        d3dImages[i].next = nullptr;
        d3dImages[i].texture = i < d3d11Textures_.size()
            ? static_cast<ID3D11Texture2D*>(d3d11Textures_[i])
            : nullptr;
    }
    return XR_SUCCESS;
}

void Swapchain::DestroyD3D11Resources()
{
    for (void* texture : d3d11Textures_)
    {
        if (texture != nullptr)
        {
            static_cast<ID3D11Texture2D*>(texture)->Release();
        }
    }
}

FrameImageSource Swapchain::SnapshotD3D11FrameImageSource(uint32_t arrayIndex) const
{
    ID3D11Texture2D* texture = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediateContext = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    std::shared_ptr<SwapchainSnapshotPoolState> pool;
    {
        std::scoped_lock lock(stateMutex_);
        if (!hasReleasedImage_ || arrayIndex >= arraySize_ ||
            lastReleasedIndex_ >= d3d11Textures_.size() ||
            d3d11Context_.device == nullptr || d3d11Context_.immediateContext == nullptr ||
            d3d11Textures_[lastReleasedIndex_] == nullptr)
        {
            return {};
        }
        texture = static_cast<ID3D11Texture2D*>(d3d11Textures_[lastReleasedIndex_]);
        texture->AddRef();
        device = d3d11Context_.device;
        device->AddRef();
        immediateContext = d3d11Context_.immediateContext;
        immediateContext->AddRef();
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
        if (device != nullptr)
        {
            device->Release();
            device = nullptr;
        }
        if (immediateContext != nullptr)
        {
            immediateContext->Release();
            immediateContext = nullptr;
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
        spdlog::debug("OXRSys: D3D11 snapshot pool is full; streaming snapshot unavailable");
        return {};
    }

    auto* source = new D3D11FrameSource();
    source->lifetime = lease;
    source->immediateContext = immediateContext;
    immediateContext = nullptr;
    source->format = format;
    source->width = width;
    source->height = height;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT result = device->CreateTexture2D(&desc, nullptr, &source->stagingTexture);
    if (FAILED(result))
    {
        spdlog::warn("OXRSys: D3D11 staging texture creation failed with {}", HResultString(result));
        releaseLocals();
        ReleaseD3D11FrameSource(source);
        return {};
    }

    const UINT sourceSubresource = D3D11CalcSubresource(0, arrayIndex, 1);
    source->immediateContext->CopySubresourceRegion(
        source->stagingTexture, 0, 0, 0, 0, texture, sourceSubresource, nullptr);
    releaseLocals();

    FrameImageSource frameSource = {};
    frameSource.api = GraphicsApi::D3D11;
    frameSource.lifetime = lease;
    frameSource.image = std::shared_ptr<void>(source, [](void* value) {
        ReleaseD3D11FrameSource(static_cast<D3D11FrameSource*>(value));
    });
    frameSource.sourceWidth = width;
    frameSource.sourceHeight = height;
    frameSource.sourceFormat = static_cast<uint64_t>(format);
    frameSource.imageWidth = width;
    frameSource.imageHeight = height;
    return frameSource;
}

void Swapchain::ReleaseD3D11FrameSource(D3D11FrameSource* source)
{
    if (source == nullptr)
    {
        return;
    }
    if (source->stagingTexture != nullptr)
    {
        source->stagingTexture->Release();
        source->stagingTexture = nullptr;
    }
    if (source->immediateContext != nullptr)
    {
        source->immediateContext->Release();
        source->immediateContext = nullptr;
    }
    delete source;
}

#endif // defined(_WIN32) && D3D11
