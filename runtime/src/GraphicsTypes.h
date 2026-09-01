// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

enum class GraphicsApi
{
    Metal,
    Vulkan,
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

struct GraphicsContext
{
    GraphicsApi api = GraphicsApi::Metal;
    void* metalDevice = nullptr;
    void* metalCommandQueue = nullptr;
    VulkanGraphicsContext vulkan = {};

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

};

enum class FrameSyncKind
{
    None,
    MetalSharedEvent,
    HostFence,
};

struct FrameSyncToken
{
    FrameSyncKind kind = FrameSyncKind::None;
    std::shared_ptr<void> waitObject = {};
    uint64_t waitValue = 0;
    std::function<bool(uint64_t)> waitForReady = {};

    bool IsValid() const
    {
        switch (kind)
        {
            case FrameSyncKind::MetalSharedEvent:
                return waitObject != nullptr && waitValue != 0;
            case FrameSyncKind::HostFence:
                return static_cast<bool>(waitForReady);
            case FrameSyncKind::None:
            default:
                return false;
        }
    }

    bool WaitForHostReady(uint64_t timeoutNs) const
    {
        return kind != FrameSyncKind::HostFence ||
               (waitForReady && waitForReady(timeoutNs));
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
