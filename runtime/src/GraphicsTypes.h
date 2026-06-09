// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <memory>

#include "VulkanGraphicsContext.h"
#if defined(_WIN32)
#include "D3DGraphicsContext.h"
#endif

enum class GraphicsApi
{
    Metal,
    Vulkan,
    D3D11,
    D3D12,
};

struct GraphicsContext
{
    GraphicsApi api = GraphicsApi::Metal;
    void* metalDevice = nullptr;
    void* metalCommandQueue = nullptr;
#ifdef XR_USE_GRAPHICS_API_VULKAN
    VulkanGraphicsContext vulkan = {};
#endif
#if defined(_WIN32)
    D3D11GraphicsContext d3d11 = {};
    D3D12GraphicsContext d3d12 = {};
#endif

    static GraphicsContext Metal(void* device, void* commandQueue = nullptr)
    {
        GraphicsContext context = {};
        context.api = GraphicsApi::Metal;
        context.metalDevice = device;
        context.metalCommandQueue = commandQueue;
        return context;
    }

#ifdef XR_USE_GRAPHICS_API_VULKAN
    static GraphicsContext Vulkan(const VulkanGraphicsContext& vulkanContext,
                                  void* debugMetalDevice = nullptr)
    {
        GraphicsContext context = {};
        context.api = GraphicsApi::Vulkan;
        context.metalDevice = debugMetalDevice;
        context.vulkan = vulkanContext;
        return context;
    }
#endif

#if defined(_WIN32)
    static GraphicsContext D3D11(const D3D11GraphicsContext& d3d11Context)
    {
        GraphicsContext context = {};
        context.api = GraphicsApi::D3D11;
        context.d3d11 = d3d11Context;
        return context;
    }

    static GraphicsContext D3D12(const D3D12GraphicsContext& d3d12Context)
    {
        GraphicsContext context = {};
        context.api = GraphicsApi::D3D12;
        context.d3d12 = d3d12Context;
        return context;
    }
#endif
};

struct FrameSyncToken
{
    GraphicsApi api = GraphicsApi::Metal;
    std::shared_ptr<void> waitObject = {};
    uint64_t waitValue = 0;

    bool IsValid() const
    {
        return waitObject != nullptr && waitValue != 0;
    }
};

struct FrameImageSource
{
    GraphicsApi api = GraphicsApi::Metal;
    std::shared_ptr<void> image = {};
    FrameSyncToken sync = {};
    std::shared_ptr<void> lifetime = {};

    void* GetImage() const
    {
        return image.get();
    }

    bool IsValid() const
    {
        return image != nullptr;
    }

    void Reset()
    {
        image.reset();
        sync = {};
        lifetime.reset();
    }
};

struct FrameSource
{
    FrameImageSource left = {};
    FrameImageSource right = {};

    bool IsStereoValid() const
    {
        return left.IsValid() && right.IsValid();
    }

    void Reset()
    {
        left.Reset();
        right.Reset();
    }
};
