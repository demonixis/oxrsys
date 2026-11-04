# SPDX-License-Identifier: MPL-2.0
#
# Native-arm64 out-of-process video encoder helper (oxrsys-encoder-helper).
#
# Part of the standard build: runtime/CMakeLists.txt adds this directory, places
# the executable next to liboxrsys-runtime.dylib (where the runtime looks for it)
# and makes oxrsys_runtime depend on it, so `cmake --build` and
# scripts/macos_build_package.sh build it with the runtime.
#
# The helper is ALWAYS arm64, whatever CMAKE_OSX_ARCHITECTURES the runtime is
# configured with. That is the point of it: the runtime dylib is loaded by an
# x86_64 (Rosetta) host that VideoToolbox refuses the hardware HEVC encoder, and
# only a native-arm64 process gets it. The per-target OSX_ARCHITECTURES below
# overrides the project-wide setting, and the helper links only system
# frameworks, so no x86_64/universal FetchContent artifact can reach its link.
#
# It can still be configured on its own (e.g. to iterate on the helper alone):
#   cmake -S runtime/encoder_helper -B build/helper -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/helper
#   lipo -info build/helper/oxrsys-encoder-helper   # must say: arm64

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Helper architecture" FORCE)
    endif()
    project(oxrsys_encoder_helper LANGUAGES CXX OBJCXX)
endif()

add_executable(oxrsys-encoder-helper
    main.mm
)

set_target_properties(oxrsys-encoder-helper PROPERTIES
    # Never x86_64, never universal: see above.
    OSX_ARCHITECTURES "arm64"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OBJCXX_STANDARD 17
    OBJCXX_STANDARD_REQUIRED ON
)

target_compile_options(oxrsys-encoder-helper PRIVATE -Wall -Wextra)

target_link_libraries(oxrsys-encoder-helper PRIVATE
    "-framework Foundation"
    "-framework CoreFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework VideoToolbox"
    "-framework IOSurface"
)

# The arm64 linker already applies a linker-signed ad-hoc signature; replace it
# with a regular ad-hoc one so the build output matches build-helper.sh and what
# the README asks for. Release packaging re-signs with a Developer ID
# (scripts/macos_sign_notarize.sh).
add_custom_command(TARGET oxrsys-encoder-helper POST_BUILD
    COMMAND /usr/bin/codesign --force --sign - $<TARGET_FILE:oxrsys-encoder-helper>
    COMMENT "Ad-hoc signing oxrsys-encoder-helper"
    VERBATIM
)
