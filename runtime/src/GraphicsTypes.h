// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

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

// A quad or cylinder composition layer, drawn into each eye's projection image
// before encode (CompositionOverlay.h has the geometry, OverlayCompositor the draw).
struct FrameOverlayLayer
{
    enum class Shape : uint32_t
    {
        Quad = 0,
        Cylinder = 1,
    };

    Shape shape = Shape::Quad;
    FrameImageSource image = {};      // the layer's swapchain snapshot + subImage rect
    float clipFromLayer[2][16] = {};  // per eye, column-major, Metal clip space
    uint32_t eyeMask = 0;             // bit 0 = left eye, bit 1 = right eye
    float width = 0.0f;               // quad: metres
    float height = 0.0f;              // quad and cylinder: metres
    float radius = 0.0f;              // cylinder
    float centralAngle = 0.0f;        // cylinder, radians
    bool sourceAlpha = false;         // XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
    bool unpremultipliedAlpha = false; // XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
};

struct FrameSource
{
    FrameImageSource left = {};
    FrameImageSource right = {};
    bool alphaBlend = false;
    std::vector<FrameOverlayLayer> overlays = {}; // back to front, all after the projection layer

    bool IsStereoValid() const
    {
        return left.IsValid() && right.IsValid();
    }

    void Reset()
    {
        left.Reset();
        right.Reset();
        alphaBlend = false;
        overlays.clear();
    }
};
