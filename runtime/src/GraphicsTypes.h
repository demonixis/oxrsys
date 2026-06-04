// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

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
    uint32_t queueFamilyIndex = 0;
    uint32_t queueIndex = 0;
};

struct GraphicsContext
{
    GraphicsApi api = GraphicsApi::Metal;
    void* metalDevice = nullptr;
    VulkanGraphicsContext vulkan = {};

    static GraphicsContext Metal(void* device)
    {
        GraphicsContext context = {};
        context.api = GraphicsApi::Metal;
        context.metalDevice = device;
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

struct FrameSource
{
    void* leftTexture = nullptr;
    void* rightTexture = nullptr;
};
