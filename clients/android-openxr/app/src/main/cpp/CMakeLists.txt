# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.22)
project(OpenXROSXClient LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENXR_OSX_PREFERRED_DISPLAY_REFRESH_RATE_HZ "72" CACHE STRING
    "Preferred headset display refresh rate requested by the Android client")

# ─── OpenXR SDK (headers + loader) ──────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    OpenXR
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.57
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(OpenXR)

# ─── Android native_app_glue ────────────────────────────────────────────────
# Provided by the NDK — gives us android_app struct and app lifecycle
set(NATIVE_APP_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")

add_library(native_app_glue STATIC
    ${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c
)
target_include_directories(native_app_glue PUBLIC
    ${NATIVE_APP_GLUE_DIR}
)
target_link_libraries(native_app_glue PUBLIC
    android
    log
)

# ─── Shared protocol definitions ────────────────────────────────────────────
set(COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../common/src")

# ─── Native client library ──────────────────────────────────────────────────
add_library(openxr_osx_client SHARED
    main.cpp
    XrApp.cpp
    VideoDecoder.cpp
    NetworkReceiver.cpp
    TrackingSender.cpp
)

target_include_directories(openxr_osx_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${COMMON_DIR}
    ${openxr_SOURCE_DIR}/include
)

target_link_libraries(openxr_osx_client PRIVATE
    -Wl,--whole-archive native_app_glue -Wl,--no-whole-archive
    android
    log
    mediandk
    nativewindow
    EGL
    GLESv3
    openxr_loader
)

target_compile_definitions(openxr_osx_client PRIVATE
    XR_USE_PLATFORM_ANDROID
    XR_USE_GRAPHICS_API_OPENGL_ES
    OPENXR_OSX_PREFERRED_DISPLAY_REFRESH_RATE_HZ=${OPENXR_OSX_PREFERRED_DISPLAY_REFRESH_RATE_HZ}
)
