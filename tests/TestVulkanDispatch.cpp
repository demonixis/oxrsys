// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "VulkanDispatch.h"

#ifdef XR_USE_GRAPHICS_API_VULKAN

namespace
{

PFN_vkVoidFunction VKAPI_PTR StubGetInstanceProcAddr(VkInstance, const char*)
{
    return nullptr;
}

} // namespace

TEST_CASE("Vulkan resolver prefers app-provided dispatch entry point", "[vulkan]")
{
    VulkanDispatch dispatch = {};
    dispatch.getInstanceProcAddr = StubGetInstanceProcAddr;

    CHECK(ResolveVulkanGetInstanceProcAddrFromProcess(dispatch) == StubGetInstanceProcAddr);
}

#endif
