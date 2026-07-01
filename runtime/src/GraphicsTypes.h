// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <memory>

#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(OXRSYS_USE_D3D12) || \
                        defined(XR_USE_GRAPHICS_API_D3D11) || defined(XR_USE_GRAPHICS_API_D3D12))
#include "D3DGraphicsContext.h"
#endif

enum class GraphicsApi
{
    Metal,
    Vulkan,
    OpenGL,
    D3D11,
    D3D12,
};

struct VulkanGraphicsContext
{
    void* instance = nullptr;
    void* physicalDevice = nullptr;
    void* device = nullptr;
    void* queue = nullptr;
    uint32_t queueFamilyIndex = 0;
    uint32_t queueIndex = 0;
};

struct OpenGLGraphicsContext
{
    void* display = nullptr;
    void* drawable = nullptr;
    void* context = nullptr;
    uint64_t visualId = 0;
};

struct GraphicsContext
{
    GraphicsApi api = GraphicsApi::Metal;
    void* metalDevice = nullptr;
    void* metalCommandQueue = nullptr;
    VulkanGraphicsContext vulkan = {};
    OpenGLGraphicsContext openGL = {};
#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(OXRSYS_USE_D3D12) || \
                        defined(XR_USE_GRAPHICS_API_D3D11) || defined(XR_USE_GRAPHICS_API_D3D12))
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

    static GraphicsContext Vulkan(const VulkanGraphicsContext& vulkanContext,
                                  void* debugMetalDevice = nullptr)
    {
        GraphicsContext context = {};
        context.api = GraphicsApi::Vulkan;
        context.metalDevice = debugMetalDevice;
        context.vulkan = vulkanContext;
        return context;
    }

    static GraphicsContext OpenGL(const OpenGLGraphicsContext& openGLContext)
    {
        GraphicsContext context = {};
        context.api = GraphicsApi::OpenGL;
        context.openGL = openGLContext;
        return context;
    }

#if defined(_WIN32) && (defined(OXRSYS_USE_D3D11) || defined(OXRSYS_USE_D3D12) || \
                        defined(XR_USE_GRAPHICS_API_D3D11) || defined(XR_USE_GRAPHICS_API_D3D12))
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
    uint32_t sourceX = 0;
    uint32_t sourceY = 0;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint64_t sourceFormat = 0;
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;

    void* GetImage() const
    {
        return image.get();
    }

    bool IsValid() const
    {
        return image != nullptr;
    }

    bool HasSourceRect() const
    {
        return sourceWidth != 0 && sourceHeight != 0;
    }

    void Reset()
    {
        image.reset();
        sync = {};
        lifetime.reset();
        sourceX = 0;
        sourceY = 0;
        sourceWidth = 0;
        sourceHeight = 0;
        sourceFormat = 0;
        imageWidth = 0;
        imageHeight = 0;
    }
};

struct FrameSource
{
    FrameImageSource left = {};
    FrameImageSource right = {};
    bool alphaBlend = false;

    bool IsStereoValid() const
    {
        return left.IsValid() && right.IsValid();
    }

    void Reset()
    {
        left.Reset();
        right.Reset();
        alphaBlend = false;
    }
};
