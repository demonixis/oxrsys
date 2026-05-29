# SPDX-License-Identifier: MPL-2.0

set(OXRSYS_RUNTIME_SOURCES
    src/Config.cpp
    src/EntryPoint.cpp
    src/Runtime.cpp
    src/RuntimeStatus.cpp
    src/Instance.cpp
    src/Session.cpp
    src/Space.cpp
    src/ActionSet.cpp
    src/InputManager.cpp
    src/HandTracker.cpp
    src/StreamingServer.cpp
    src/TrackingReceiver.cpp
)

if(APPLE)
    list(APPEND OXRSYS_RUNTIME_SOURCES
        src/Swapchain.mm
        src/VideoEncoder.mm
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND OXRSYS_RUNTIME_SOURCES
        src/SwapchainVulkan.cpp
        src/FfmpegVideoEncoder.cpp
    )
else()
    message(FATAL_ERROR
        "The ${CMAKE_SYSTEM_NAME} runtime backend is scaffold-only in this pass. "
        "Use macOS or Linux for runtime builds."
    )
endif()

add_library(oxrsys_runtime SHARED
    ${OXRSYS_RUNTIME_SOURCES}
)

find_package(Vulkan REQUIRED)

set(OXRSYS_RUNTIME_LINK_LIBRARIES
    spdlog::spdlog
    glm::glm
    ${CMAKE_DL_LIBS}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec
        libavutil
        libswscale
    )
    list(APPEND OXRSYS_RUNTIME_LINK_LIBRARIES
        PkgConfig::FFMPEG
    )
endif()

target_include_directories(oxrsys_runtime PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/common/protocol/include
    ${openxr_SOURCE_DIR}/include
    ${openxr_SOURCE_DIR}/src/common
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(oxrsys_runtime PRIVATE
    XR_USE_GRAPHICS_API_VULKAN
)

if(APPLE)
    target_compile_definitions(oxrsys_runtime PRIVATE
        XR_USE_GRAPHICS_API_METAL
    )
endif()

# NOTE: We intentionally do NOT link against Vulkan::Vulkan.
# Linking the Vulkan loader would load the system MoltenVK ICD, which conflicts
# with app-embedded MoltenVK (e.g. Godot). Instead, all Vulkan functions are
# resolved dynamically through the app's pfnGetInstanceProcAddr.
target_link_libraries(oxrsys_runtime PRIVATE
    ${OXRSYS_RUNTIME_LINK_LIBRARIES}
)

if(APPLE)
    target_link_libraries(oxrsys_runtime PRIVATE
        "-framework Metal"
        "-framework MetalPerformanceShaders"
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework AppKit"
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
    )
endif()

# Only export the negotiation entry point
set_target_properties(oxrsys_runtime PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME oxrsys-runtime
    PREFIX "lib"
)

if(APPLE)
    set_target_properties(oxrsys_runtime PROPERTIES
        OBJCXX_VISIBILITY_PRESET hidden
        SUFFIX ".dylib"
    )
endif()

set(OXRSYS_RUNTIME_LIBRARY_FILENAME "$<TARGET_FILE_NAME:oxrsys_runtime>")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/oxrsys-runtime.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/oxrsys-runtime.json.in.configured
    @ONLY
)

# Copy manifest JSON next to the dylib
file(GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/oxrsys-runtime.json
    INPUT ${CMAKE_CURRENT_BINARY_DIR}/oxrsys-runtime.json.in.configured
)

# Copy default config file next to the dylib (if not already present)
add_custom_command(TARGET oxrsys_runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/oxrsys-runtime.toml
        ${CMAKE_CURRENT_BINARY_DIR}/oxrsys-runtime.toml
    COMMENT "Copying oxrsys-runtime.toml to build directory"
)
