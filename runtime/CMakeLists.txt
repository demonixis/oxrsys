# SPDX-License-Identifier: MPL-2.0

add_library(oxrsys_runtime SHARED
    src/Config.cpp
    src/EntryPoint.cpp
    src/Runtime.cpp
    src/RuntimeStatus.cpp
    src/Instance.cpp
    src/Session.cpp
    src/Swapchain.mm
    src/Space.cpp
    src/ActionSet.cpp
    src/InputManager.cpp
    src/HandTracker.cpp
    src/StreamingServer.cpp
    src/TrackingReceiver.cpp
    src/VideoEncoder.mm
)

find_package(Vulkan REQUIRED)

target_include_directories(oxrsys_runtime PRIVATE
    src
    ${openxr_SOURCE_DIR}/include
    ${openxr_SOURCE_DIR}/src/common
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(oxrsys_runtime PRIVATE
    XR_USE_GRAPHICS_API_METAL
    XR_USE_GRAPHICS_API_VULKAN
)

# NOTE: We intentionally do NOT link against Vulkan::Vulkan.
# Linking the Vulkan loader would load the system MoltenVK ICD, which conflicts
# with app-embedded MoltenVK (e.g. Godot). Instead, all Vulkan functions are
# resolved dynamically through the app's pfnGetInstanceProcAddr.
target_link_libraries(oxrsys_runtime PRIVATE
    spdlog::spdlog
    glm::glm
    "-framework Metal"
    "-framework MetalPerformanceShaders"
    "-framework Foundation"
    "-framework QuartzCore"
    "-framework AppKit"
    "-framework VideoToolbox"
    "-framework CoreMedia"
    "-framework CoreVideo"
)

# Only export the negotiation entry point
set_target_properties(oxrsys_runtime PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    OBJCXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME oxrsys-runtime
    PREFIX "lib"
    SUFFIX ".dylib"
)

# Copy manifest JSON next to the dylib
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/oxrsys-runtime.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/oxrsys-runtime.json
    @ONLY
)

# Copy default config file next to the dylib (if not already present)
add_custom_command(TARGET oxrsys_runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/oxrsys-runtime.toml
        ${CMAKE_CURRENT_BINARY_DIR}/oxrsys-runtime.toml
    COMMENT "Copying oxrsys-runtime.toml to build directory"
)
