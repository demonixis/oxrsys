# SPDX-License-Identifier: MPL-2.0

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "OXRSys is a macOS-only OpenXR runtime")
endif()

add_library(oxrsys_runtime SHARED
    src/Config.cpp
    src/EntryPoint.cpp
    src/Runtime.cpp
    src/RuntimeStatus.cpp
    src/RuntimePlatform.cpp
    src/RuntimeSockets.cpp
    src/StreamingFrameQueue.cpp
    src/VideoCodecSelection.cpp
    src/Instance.cpp
    src/Session.cpp
    src/Space.cpp
    src/ActionSet.cpp
    src/InputManager.cpp
    src/HandTracker.cpp
    src/StreamingServer.cpp
    src/TrackingReceiver.cpp
    src/VulkanDispatch.cpp
    src/SwapchainCommon.cpp
    src/SwapchainVulkan.cpp
    src/SwapchainMetal.mm
    src/VideoEncoder.mm
    src/EncoderHelperClient.mm
    src/EncoderPathPolicy.cpp
)

target_include_directories(oxrsys_runtime PRIVATE
    src
    encoder_helper
    ${CMAKE_SOURCE_DIR}/common/protocol/include
    ${openxr_SOURCE_DIR}/include
    ${openxr_SOURCE_DIR}/src/common
)

target_compile_definitions(oxrsys_runtime PRIVATE
    XR_USE_GRAPHICS_API_METAL
    XR_USE_GRAPHICS_API_VULKAN
    OXRSYS_USE_METAL
    OXRSYS_USE_VULKAN
    OXRSYS_VERSION_MAJOR=${OXRSYS_VERSION_MAJOR}
    OXRSYS_VERSION_MINOR=${OXRSYS_VERSION_MINOR}
    OXRSYS_VERSION_PATCH=${OXRSYS_VERSION_PATCH}
)

# Vulkan is intentionally headers-only here. The runtime resolves every Vulkan
# function through the application-provided loader dispatch (with the existing
# process-local vkGetInstanceProcAddr fallback) and never links a Vulkan loader.
target_link_libraries(oxrsys_runtime PRIVATE
    spdlog::spdlog
    glm::glm
    OXRSYS::VulkanHeaders
    ${CMAKE_DL_LIBS}
    "-framework Metal"
    "-framework MetalPerformanceShaders"
    "-framework Foundation"
    "-framework QuartzCore"
    "-framework VideoToolbox"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework IOSurface"
)

# Only export the negotiation entry point.
set_target_properties(oxrsys_runtime PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    OBJCXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME oxrsys-runtime
    PREFIX "lib"
    SUFFIX ".dylib"
)

set(OXRSYS_RUNTIME_LIBRARY_FILENAME "$<TARGET_FILE_NAME:oxrsys_runtime>")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/oxrsys-runtime.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/oxrsys-runtime.json.in.configured
    @ONLY
)

file(GENERATE
    OUTPUT $<TARGET_FILE_DIR:oxrsys_runtime>/oxrsys-runtime.json
    INPUT ${CMAKE_CURRENT_BINARY_DIR}/oxrsys-runtime.json.in.configured
)

add_custom_command(TARGET oxrsys_runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/oxrsys-runtime.toml
        ${CMAKE_CURRENT_BINARY_DIR}/oxrsys-runtime.toml
    COMMENT "Copying oxrsys-runtime.toml to build directory"
)

# --- Native-arm64 encoder helper ---
# Always built arm64, even when this runtime is x86_64 or universal (see
# encoder_helper/CMakeLists.txt). It is placed next to liboxrsys-runtime.dylib,
# which is where VideoEncoder looks for it when encoder_helper_path is empty,
# and oxrsys_runtime depends on it so building the runtime target alone (as the
# packaging scripts do) always produces a matching helper.
add_subdirectory(encoder_helper)
set_target_properties(oxrsys-encoder-helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:oxrsys_runtime>"
)
add_dependencies(oxrsys_runtime oxrsys-encoder-helper)
