# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.24)

set(OXRSYS_VERSION_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config/OXRSysVersion.xcconfig")
file(STRINGS "${OXRSYS_VERSION_CONFIG_FILE}" OXRSYS_VERSION_CONFIG_LINES)
foreach(oxrsys_version_config_line IN LISTS OXRSYS_VERSION_CONFIG_LINES)
    string(REGEX REPLACE "//.*$" "" oxrsys_version_config_line "${oxrsys_version_config_line}")
    string(STRIP "${oxrsys_version_config_line}" oxrsys_version_config_line)
    if(oxrsys_version_config_line MATCHES "^([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*(.*)$")
        set("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    endif()
endforeach()

if(NOT "${OXRSYS_VERSION}" MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "OXRSYS_VERSION must use MAJOR.MINOR.PATCH in ${OXRSYS_VERSION_CONFIG_FILE}")
endif()
set(OXRSYS_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(OXRSYS_VERSION_MINOR "${CMAKE_MATCH_2}")
set(OXRSYS_VERSION_PATCH "${CMAKE_MATCH_3}")

if(NOT "${OXRSYS_BUILD}" MATCHES "^[0-9]+$")
    message(FATAL_ERROR "OXRSYS_BUILD must be an integer in ${OXRSYS_VERSION_CONFIG_FILE}")
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin"
        AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING
        "Minimum supported macOS version" FORCE)
endif()

project(OXRSys VERSION "${OXRSYS_VERSION}" LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "OXRSys is a macOS-only OpenXR runtime")
endif()

foreach(oxrsys_removed_option IN ITEMS
        OXRSYS_BUILD_QT_FRONTENDS
        OXRSYS_VIDEO_ENCODER
        OXRSYS_LIBVA_PROVIDER)
    if(DEFINED ${oxrsys_removed_option})
        message(FATAL_ERROR
            "${oxrsys_removed_option} was removed from the macOS-only runtime"
        )
    endif()
endforeach()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_language(OBJCXX)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
enable_testing()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${CMAKE_BINARY_DIR}/compile_commands.json
        ${CMAKE_SOURCE_DIR}/compile_commands.json
    RESULT_VARIABLE oxrsys_runtime_compile_commands_symlink_result
)

if(NOT oxrsys_runtime_compile_commands_symlink_result EQUAL 0)
    message(STATUS "Failed to create compile_commands.json symlink during configure. The build sync step will retry.")
endif()

# --- FetchContent dependencies ---
include(FetchContent)

# spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
    GIT_SHALLOW ON
)
set(SPDLOG_BUILD_PIC ON CACHE BOOL "Build spdlog with position independent code" FORCE)

# glm
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW ON
)

FetchContent_MakeAvailable(spdlog glm)

# Catch2 for tests
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(Catch2)

# OpenXR SDK (loader + headers)
FetchContent_Declare(
    OpenXR
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.57
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(OpenXR)

# Header-only Vulkan discovery. OXRSys must use the application's Vulkan
# dispatch and therefore intentionally does not link a Vulkan loader.
find_path(OXRSYS_VULKAN_INCLUDE_DIR
    NAMES vulkan/vulkan.h
    HINTS
        "$ENV{VULKAN_SDK}"
        /opt/homebrew
        /usr/local
    PATH_SUFFIXES include Include
    REQUIRED
)
add_library(oxrsys_vulkan_headers INTERFACE)
target_include_directories(oxrsys_vulkan_headers INTERFACE
    "${OXRSYS_VULKAN_INCLUDE_DIR}"
)
add_library(OXRSYS::VulkanHeaders ALIAS oxrsys_vulkan_headers)

# --- Our runtime ---
add_subdirectory(runtime)

add_custom_target(oxrsys_runtime_sync_compile_commands ALL
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_BINARY_DIR}/compile_commands.json
        -DOUTPUT_FILE=${CMAKE_SOURCE_DIR}/compile_commands.json
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/SyncCompileCommands.cmake
    COMMENT "Syncing compile_commands.json to the project root"
)

# --- Tests ---
set(OXRSYS_TEST_ENV_ROOT "${CMAKE_BINARY_DIR}/test-env")
set(OXRSYS_TEST_HOME "${OXRSYS_TEST_ENV_ROOT}/home")
file(MAKE_DIRECTORY "${OXRSYS_TEST_HOME}")
set(OXRSYS_ISOLATED_TEST_ENV
    "HOME=${OXRSYS_TEST_HOME}"
)

set(OXRSYS_LOGCAT_TEST_HOME "${OXRSYS_TEST_ENV_ROOT}/home-logcat")
set(OXRSYS_LOGCAT_TEST_CONFIG_DIR
    "${OXRSYS_LOGCAT_TEST_HOME}/Library/Application Support/OXRSys"
)
set(OXRSYS_FAKE_ADB "${OXRSYS_TEST_ENV_ROOT}/fake-adb")
file(MAKE_DIRECTORY "${OXRSYS_LOGCAT_TEST_CONFIG_DIR}")
file(WRITE "${OXRSYS_LOGCAT_TEST_CONFIG_DIR}/oxrsys-runtime.toml"
    "[logging]\n"
    "file_logging = false\n"
    "quest_logcat = true\n"
)
file(WRITE "${OXRSYS_FAKE_ADB}"
    "#!/bin/sh\n"
    "exec sleep 30\n"
)
file(CHMOD "${OXRSYS_FAKE_ADB}"
    PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE
)
set(OXRSYS_LOGCAT_TEST_ENV
    "HOME=${OXRSYS_LOGCAT_TEST_HOME}"
    "OXRSYS_ADB_PATH=${OXRSYS_FAKE_ADB}"
)

add_executable(oxrsys_runtime_tests
    tests/TestBoundedDrain.cpp
    tests/TestClientLiveness.cpp
    tests/TestConfig.cpp
    tests/TestInputManager.cpp
    tests/TestRuntimePlatform.cpp
    tests/TestStreamingAbr.cpp
    tests/TestStreamingFrameQueue.cpp
    tests/TestStreamingReconfigure.cpp
    tests/TestSnapshotLeasePool.cpp
    tests/TestSwapchainCreateValidation.cpp
    tests/TestVideoCodecSelection.cpp
    tests/TestVideoTextureFormat.cpp
    tests/TestVulkanDispatch.cpp
    tests/TestCompositionLayerAlpha.cpp
    tests/TestProtocolLayout.cpp
    tests/TestQuestPassthroughAlphaPolicy.cpp
    tests/TestQuestShellInteraction.cpp
    clients/android-vr/app/src/main/cpp/QuestPassthroughAlphaPolicy.cpp
    clients/android-vr/app/src/main/cpp/QuestShellInteraction.cpp
    runtime/src/InputManager.cpp
    runtime/src/TrackingReceiver.cpp
    runtime/src/Config.cpp
    runtime/src/RuntimePlatform.cpp
    runtime/src/RuntimeSockets.cpp
    runtime/src/StreamingFrameQueue.cpp
    runtime/src/VideoCodecSelection.cpp
    runtime/src/VulkanDispatch.cpp
)
target_include_directories(oxrsys_runtime_tests PRIVATE
    runtime/src
    clients/android-vr/app/src/main/cpp
    common/protocol/include
    ${openxr_SOURCE_DIR}/include
)
target_link_libraries(oxrsys_runtime_tests PRIVATE
    Catch2::Catch2WithMain
    glm::glm
    spdlog::spdlog
    OXRSYS::VulkanHeaders
    ${CMAKE_DL_LIBS}
)
target_compile_definitions(oxrsys_runtime_tests PRIVATE
    XR_USE_GRAPHICS_API_METAL
    XR_USE_GRAPHICS_API_VULKAN
)
add_test(NAME oxrsys_runtime_tests COMMAND oxrsys_runtime_tests)
set_tests_properties(oxrsys_runtime_tests PROPERTIES
    ENVIRONMENT "${OXRSYS_ISOLATED_TEST_ENV}"
    TIMEOUT 120
)
add_test(NAME oxrsys_runtime_logcat_timeout
    COMMAND oxrsys_runtime_tests "Config singleton initializes with bounded Quest logcat clear"
)
set_tests_properties(oxrsys_runtime_logcat_timeout PROPERTIES
    ENVIRONMENT "${OXRSYS_LOGCAT_TEST_ENV}"
    TIMEOUT 10
)

add_executable(oxrsys_runtime_status_tests
    tests/TestRuntimeStatus.cpp
    runtime/src/RuntimeStatus.cpp
    runtime/src/Config.cpp
    runtime/src/RuntimePlatform.cpp
)
target_include_directories(oxrsys_runtime_status_tests PRIVATE
    runtime/src
    common/protocol/include
)
target_link_libraries(oxrsys_runtime_status_tests PRIVATE
    Catch2::Catch2WithMain
    spdlog::spdlog
    ${CMAKE_DL_LIBS}
)
add_test(NAME oxrsys_runtime_status_tests COMMAND oxrsys_runtime_status_tests)
set_tests_properties(oxrsys_runtime_status_tests PROPERTIES
    ENVIRONMENT "${OXRSYS_ISOLATED_TEST_ENV}"
    TIMEOUT 120
)

add_executable(oxrsys_runtime_loader_extension_tests
    tests/TestRuntimeLoaderExtensions.cpp
)
target_include_directories(oxrsys_runtime_loader_extension_tests PRIVATE
    ${openxr_SOURCE_DIR}/include
)
target_link_libraries(oxrsys_runtime_loader_extension_tests PRIVATE
    Catch2::Catch2WithMain
    OpenXR::openxr_loader
)
add_dependencies(oxrsys_runtime_loader_extension_tests oxrsys_runtime)
add_test(NAME oxrsys_runtime_loader_extension_tests
    COMMAND oxrsys_runtime_loader_extension_tests
)
set_tests_properties(oxrsys_runtime_loader_extension_tests PROPERTIES
    ENVIRONMENT
        "HOME=${OXRSYS_TEST_HOME};XR_RUNTIME_JSON=$<TARGET_FILE_DIR:oxrsys_runtime>/oxrsys-runtime.json"
    TIMEOUT 120
)

set(OXRSYS_RUNTIME_TEST_TARGETS
    oxrsys_runtime
    oxrsys_runtime_tests
    oxrsys_runtime_status_tests
    oxrsys_runtime_loader_extension_tests
)

add_executable(oxrsys_runtime_api_tests
    tests/TestRuntimeApi.cpp
    tests/TestMetalSupport.mm
)
target_include_directories(oxrsys_runtime_api_tests PRIVATE
    runtime/src
    ${openxr_SOURCE_DIR}/include
)
target_link_libraries(oxrsys_runtime_api_tests PRIVATE
    Catch2::Catch2WithMain
    OpenXR::openxr_loader
    "-framework Metal"
    "-framework Foundation"
)
target_compile_definitions(oxrsys_runtime_api_tests PRIVATE
    XR_USE_GRAPHICS_API_METAL
    OXRSYS_VERSION_MAJOR=${OXRSYS_VERSION_MAJOR}
    OXRSYS_VERSION_MINOR=${OXRSYS_VERSION_MINOR}
    OXRSYS_VERSION_PATCH=${OXRSYS_VERSION_PATCH}
)
add_dependencies(oxrsys_runtime_api_tests oxrsys_runtime)
add_test(NAME oxrsys_runtime_api_tests COMMAND oxrsys_runtime_api_tests)
set_tests_properties(oxrsys_runtime_api_tests PROPERTIES
    ENVIRONMENT
        "HOME=${OXRSYS_TEST_HOME};XR_RUNTIME_JSON=$<TARGET_FILE_DIR:oxrsys_runtime>/oxrsys-runtime.json"
    TIMEOUT 120
)
list(APPEND OXRSYS_RUNTIME_TEST_TARGETS oxrsys_runtime_api_tests)

add_executable(oxrsys_videotoolbox_encoder_tests
    tests/TestVideoToolboxEncoder.mm
    runtime/src/VideoEncoder.mm
    runtime/src/Config.cpp
    runtime/src/RuntimePlatform.cpp
)
target_include_directories(oxrsys_videotoolbox_encoder_tests PRIVATE
    runtime/src
    common/protocol/include
    ${openxr_SOURCE_DIR}/include
)
target_link_libraries(oxrsys_videotoolbox_encoder_tests PRIVATE
    Catch2::Catch2WithMain
    spdlog::spdlog
    "-framework Metal"
    "-framework MetalPerformanceShaders"
    "-framework Foundation"
    "-framework VideoToolbox"
    "-framework CoreMedia"
    "-framework CoreVideo"
)
target_compile_definitions(oxrsys_videotoolbox_encoder_tests PRIVATE
    XR_USE_GRAPHICS_API_METAL
    OXRSYS_VERSION_MAJOR=${OXRSYS_VERSION_MAJOR}
    OXRSYS_VERSION_MINOR=${OXRSYS_VERSION_MINOR}
    OXRSYS_VERSION_PATCH=${OXRSYS_VERSION_PATCH}
)
add_test(NAME oxrsys_videotoolbox_encoder_tests
    COMMAND oxrsys_videotoolbox_encoder_tests
)
set_tests_properties(oxrsys_videotoolbox_encoder_tests PROPERTIES
    ENVIRONMENT "${OXRSYS_ISOLATED_TEST_ENV}"
    TIMEOUT 30
)
list(APPEND OXRSYS_RUNTIME_TEST_TARGETS oxrsys_videotoolbox_encoder_tests)

add_dependencies(oxrsys_runtime_sync_compile_commands
    ${OXRSYS_RUNTIME_TEST_TARGETS}
)

option(OXRSYS_ENABLE_CTS "Fetch and wire an opt-in OpenXR CTS lane" OFF)

add_custom_target(oxrsys_runtime_set_default_runtime
    COMMAND /bin/zsh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/oxrsys_runtime_default.sh set $<TARGET_FILE_DIR:oxrsys_runtime>/oxrsys-runtime.json
    DEPENDS oxrsys_runtime
    USES_TERMINAL
    COMMENT "Registering the generated OpenXR manifest as the default runtime for this user"
)

if(OXRSYS_ENABLE_CTS)
    include(ExternalProject)

    set(OXRSYS_CTS_TAG "openxr-cts-1.1.57.0" CACHE STRING "Pinned OpenXR CTS tag")
    set(OXRSYS_CTS_GRAPHICS_PLUGIN "metal" CACHE STRING "Graphics plugin to use for the CTS baseline")
    set(OXRSYS_CTS_FILTER "exclude:[interactive]" CACHE STRING "Catch2 filter used for the CTS baseline")
    set(OXRSYS_CTS_PREFIX_DIR "${CMAKE_BINARY_DIR}/thirdparty/OpenXR-CTS")
    set(OXRSYS_CTS_SOURCE_DIR "${OXRSYS_CTS_PREFIX_DIR}/source")
    set(OXRSYS_CTS_BINARY_DIR "${OXRSYS_CTS_PREFIX_DIR}/build")
    set(OXRSYS_CTS_REPORT_DIR "${CMAKE_BINARY_DIR}/reports/openxr-cts")
    set(OXRSYS_CTS_CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    )
    if(APPLE)
        execute_process(
            COMMAND xcrun --find metal
            RESULT_VARIABLE OXRSYS_CTS_METAL_RESULT
            OUTPUT_VARIABLE OXRSYS_CTS_METAL_EXECUTABLE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(OXRSYS_CTS_METAL_RESULT EQUAL 0 AND OXRSYS_CTS_METAL_EXECUTABLE)
            set(MetalTools_METAL_EXECUTABLE "${OXRSYS_CTS_METAL_EXECUTABLE}" CACHE FILEPATH "Path to the Apple metal compiler" FORCE)
        endif()
        execute_process(
            COMMAND xcrun --find metallib
            RESULT_VARIABLE OXRSYS_CTS_METALLIB_RESULT
            OUTPUT_VARIABLE OXRSYS_CTS_METALLIB_EXECUTABLE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(OXRSYS_CTS_METALLIB_RESULT EQUAL 0 AND OXRSYS_CTS_METALLIB_EXECUTABLE)
            set(MetalTools_METALLIB_EXECUTABLE "${OXRSYS_CTS_METALLIB_EXECUTABLE}" CACHE FILEPATH "Path to the Apple metallib compiler" FORCE)
        endif()
        if(MetalTools_METAL_EXECUTABLE AND MetalTools_METALLIB_EXECUTABLE)
            list(APPEND OXRSYS_CTS_CMAKE_ARGS
                -DMetalTools_METAL_EXECUTABLE=${MetalTools_METAL_EXECUTABLE}
                -DMetalTools_METALLIB_EXECUTABLE=${MetalTools_METALLIB_EXECUTABLE}
            )
        else()
            message(WARNING "Could not resolve metal and metallib with xcrun; install the Xcode Metal Toolchain before running the CTS Metal target.")
        endif()
    endif()

    ExternalProject_Add(openxr_cts
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-CTS.git
        GIT_TAG ${OXRSYS_CTS_TAG}
        GIT_SHALLOW ON
        PREFIX "${OXRSYS_CTS_PREFIX_DIR}"
        SOURCE_DIR "${OXRSYS_CTS_SOURCE_DIR}"
        BINARY_DIR "${OXRSYS_CTS_BINARY_DIR}"
        CMAKE_ARGS
            ${OXRSYS_CTS_CMAKE_ARGS}
        UPDATE_DISCONNECTED ON
        INSTALL_COMMAND ""
        EXCLUDE_FROM_ALL ON
    )

    add_custom_target(openxr_cts_run
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OXRSYS_CTS_REPORT_DIR}"
        COMMAND ${CMAKE_COMMAND}
            -DCTS_BINARY_DIR=${OXRSYS_CTS_BINARY_DIR}
            -DREPORT_DIR=${OXRSYS_CTS_REPORT_DIR}
            -DRUNTIME_JSON=$<TARGET_FILE_DIR:oxrsys_runtime>/oxrsys-runtime.json
            -DCTS_GRAPHICS_PLUGIN=${OXRSYS_CTS_GRAPHICS_PLUGIN}
            -DCTS_FILTER=${OXRSYS_CTS_FILTER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunOpenXRCTS.cmake
        DEPENDS openxr_cts oxrsys_runtime
        USES_TERMINAL
        COMMENT "Running the pinned OpenXR CTS baseline against the OXRSys runtime"
    )
endif()
