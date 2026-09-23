# Steam Frame client
# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.21)
project(oxrsys_steam_frame_client LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

# OpenXR: prefer a real CONFIG package (the SDK's), else a loader library +
# headers. Provide OPENXR_INCLUDE_DIR and/or OPENXR_LOADER_DIR if not on the
# default search paths. On the Frame these come from the OpenXR SDK.
set(OPENXR_INCLUDE_DIR "" CACHE PATH "OpenXR headers (dir containing openxr/openxr.h)")
set(OPENXR_FOUND FALSE)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
endif()
if(NOT OPENXR_FOUND)
    find_library(OPENXR_LOADER_LIB NAMES openxr_loader HINTS "$ENV{OPENXR_LOADER_DIR}")
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        set_target_properties(OpenXR::openxr_loader PROPERTIES
            IMPORTED_LOCATION "${OPENXR_LOADER_LIB}")
    endif()
endif()
if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Install the OpenXR SDK or set OPENXR_LOADER_DIR.")
endif()

# FFmpeg (decode) via pkg-config.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswscale)

# Compile shaders to SPIR-V at build time.
find_program(GLSLANG glslangValidator REQUIRED)
set(SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT}")
set(SPV_FILES)
foreach(sh fullscreen.vert sample.frag)
    add_custom_command(
        OUTPUT "${SHADER_OUT}/${sh}.spv"
        COMMAND ${GLSLANG} -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${sh}"
                -o "${SHADER_OUT}/${sh}.spv"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${sh}"
        COMMENT "glslang ${sh}")
    list(APPEND SPV_FILES "${SHADER_OUT}/${sh}.spv")
endforeach()
add_custom_target(steam_frame_client_shaders DEPENDS ${SPV_FILES})

# The canonical in-tree oxrsys protocol lib (header-only).
set(OXRSYS_PROTO_INC "${CMAKE_CURRENT_SOURCE_DIR}/../../common/protocol/include")
set(OXRSYS_STREAMING_INC "${CMAKE_CURRENT_SOURCE_DIR}/../../common/streaming/include")

set(CLIENT_SOURCES
    src/main.cpp
    src/video_decoder.cpp
    src/stream_connection.cpp
    src/NetworkReceiver.cpp
    src/TrackingSender.cpp
    src/xr_input.cpp)

add_executable(steam_frame_client ${CLIENT_SOURCES})
add_dependencies(steam_frame_client steam_frame_client_shaders)
target_include_directories(steam_frame_client PRIVATE
    ${OPENXR_INCLUDE_DIR} ${OXRSYS_PROTO_INC} ${OXRSYS_STREAMING_INC} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(steam_frame_client PRIVATE
    FRAME_CLIENT_SHADER_DIR="${SHADER_OUT}")
target_link_libraries(steam_frame_client PRIVATE
    OpenXR::openxr_loader Vulkan::Vulkan PkgConfig::FFMPEG)

# ---- Tests (headless: no OpenXR/Vulkan runtime needed) ----
enable_testing()
add_executable(steam_frame_client_tests
    tests/test_main.cpp
    tests/test_video_decoder.cpp
    tests/test_protocol_fec.cpp
    tests/test_foveation.cpp
    src/video_decoder.cpp)
target_include_directories(steam_frame_client_tests PRIVATE
    ${OXRSYS_PROTO_INC} ${OXRSYS_STREAMING_INC} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(steam_frame_client_tests PRIVATE PkgConfig::FFMPEG)
target_compile_definitions(steam_frame_client_tests PRIVATE
    TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
add_test(NAME steam_frame_client_tests COMMAND steam_frame_client_tests)
