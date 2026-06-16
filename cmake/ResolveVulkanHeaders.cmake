# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

set(OXRSYS_VULKAN_HEADERS_TAG "vulkan-sdk-1.4.321.0" CACHE STRING
    "Vulkan-Headers tag used when no system Vulkan headers are found")

find_path(OXRSYS_VULKAN_INCLUDE_DIR
    NAMES vulkan/vulkan.h
    HINTS
        "$ENV{VULKAN_SDK}/Include"
        "$ENV{VULKAN_SDK}/include"
)

if(NOT OXRSYS_VULKAN_INCLUDE_DIR)
    include(FetchContent)
    FetchContent_Declare(
        VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG ${OXRSYS_VULKAN_HEADERS_TAG}
        GIT_SHALLOW ON
    )
    FetchContent_MakeAvailable(VulkanHeaders)

    if(EXISTS "${vulkanheaders_SOURCE_DIR}/include/vulkan/vulkan.h")
        set(OXRSYS_VULKAN_INCLUDE_DIR "${vulkanheaders_SOURCE_DIR}/include"
            CACHE PATH "Vulkan headers include directory" FORCE)
    endif()
endif()

if(NOT OXRSYS_VULKAN_INCLUDE_DIR OR
   NOT EXISTS "${OXRSYS_VULKAN_INCLUDE_DIR}/vulkan/vulkan.h")
    message(FATAL_ERROR
        "Vulkan headers were not found. Install the Vulkan SDK or allow "
        "FetchContent to download Vulkan-Headers.")
endif()

set(OXRSYS_VULKAN_INCLUDE_DIRS "${OXRSYS_VULKAN_INCLUDE_DIR}")
message(STATUS "Using Vulkan headers from ${OXRSYS_VULKAN_INCLUDE_DIR}")
