# SPDX-License-Identifier: MPL-2.0
#
# Builds ALVR's alvr_server_core Rust cdylib (pinned checkout, see
# runtime/src/alvr/alvr_server_core.h for the tag) via cargo and links it into
# oxrsys_runtime. Included from runtime/CMakeLists.txt.
#
# The Homebrew rust toolchain only ships the host (arm64) std, so this uses
# the rustup-managed toolchain, which has the x86_64-apple-darwin target the
# Wine/Rosetta runtime build needs.

option(OXRSYS_ENABLE_ALVR "Build and link the ALVR server_core streaming backend" ON)

set(OXRSYS_ALVR_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../ALVR" CACHE PATH
    "Path to the pinned ALVR checkout")

if(NOT OXRSYS_ENABLE_ALVR)
    return()
endif()

if(NOT APPLE)
    message(STATUS "OXRSYS_ENABLE_ALVR is only wired up for macOS; skipping")
    return()
endif()

if(NOT EXISTS "${OXRSYS_ALVR_SOURCE_DIR}/alvr/server_core/Cargo.toml")
    message(WARNING
        "ALVR checkout not found at ${OXRSYS_ALVR_SOURCE_DIR}; "
        "disabling the ALVR backend (set OXRSYS_ALVR_SOURCE_DIR or OXRSYS_ENABLE_ALVR=OFF)")
    return()
endif()

# Map the single configured macOS arch to a Rust target triple.
if(CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES oxrsys_alvr_arch_count)
    if(NOT oxrsys_alvr_arch_count EQUAL 1)
        message(WARNING
            "OXRSYS_ENABLE_ALVR does not support universal builds; disabling the ALVR backend")
        return()
    endif()
    set(oxrsys_alvr_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(oxrsys_alvr_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
if(oxrsys_alvr_arch STREQUAL "x86_64")
    set(OXRSYS_ALVR_RUST_TARGET "x86_64-apple-darwin")
elseif(oxrsys_alvr_arch MATCHES "^(arm64|aarch64)$")
    set(OXRSYS_ALVR_RUST_TARGET "aarch64-apple-darwin")
else()
    message(WARNING "Unsupported arch ${oxrsys_alvr_arch} for the ALVR backend; disabling")
    return()
endif()

# Locate the rustup-managed toolchain (NOT Homebrew cargo, which lacks the
# cross-target std). Overridable via OXRSYS_CARGO_BIN_DIR.
if(NOT OXRSYS_CARGO_BIN_DIR)
    file(GLOB oxrsys_rustup_toolchains "$ENV{HOME}/.rustup/toolchains/stable-*/bin")
    list(LENGTH oxrsys_rustup_toolchains oxrsys_rustup_toolchain_count)
    if(oxrsys_rustup_toolchain_count GREATER_EQUAL 1)
        list(GET oxrsys_rustup_toolchains 0 OXRSYS_CARGO_BIN_DIR)
    endif()
endif()
if(NOT OXRSYS_CARGO_BIN_DIR OR NOT EXISTS "${OXRSYS_CARGO_BIN_DIR}/cargo")
    message(WARNING
        "No rustup stable toolchain found (looked in ~/.rustup/toolchains); "
        "disabling the ALVR backend. Install with: rustup toolchain install stable "
        "&& rustup target add ${OXRSYS_ALVR_RUST_TARGET}")
    return()
endif()

set(OXRSYS_ALVR_CARGO_TARGET_DIR "${OXRSYS_ALVR_SOURCE_DIR}/target")
set(OXRSYS_ALVR_BUILT_DYLIB
    "${OXRSYS_ALVR_CARGO_TARGET_DIR}/${OXRSYS_ALVR_RUST_TARGET}/release/libalvr_server_core.dylib")
set(OXRSYS_ALVR_STAGED_DYLIB "${CMAKE_CURRENT_BINARY_DIR}/libalvr_server_core.dylib")

# Always invoke cargo (a no-op rebuild is fast); then stage the dylib next to
# liboxrsys-runtime.dylib with an @loader_path install name and an ad-hoc
# signature. Staging is skipped when cargo's output is unchanged, so the
# runtime does not relink every build. BYPRODUCTS gives ninja a rule for the
# staged dylib that the runtime link step consumes.
add_custom_target(oxrsys_alvr_server_core
    COMMAND ${CMAKE_COMMAND} -E env
        "PATH=${OXRSYS_CARGO_BIN_DIR}:$ENV{PATH}"
        "RUSTC=${OXRSYS_CARGO_BIN_DIR}/rustc"
        "${OXRSYS_CARGO_BIN_DIR}/cargo" build -p alvr_server_core --release
            --target ${OXRSYS_ALVR_RUST_TARGET}
    COMMAND ${CMAKE_COMMAND}
        -DBUILT=${OXRSYS_ALVR_BUILT_DYLIB}
        -DSTAGED=${OXRSYS_ALVR_STAGED_DYLIB}
        -P ${CMAKE_SOURCE_DIR}/cmake/AlvrStageDylib.cmake
    BYPRODUCTS "${OXRSYS_ALVR_STAGED_DYLIB}"
    WORKING_DIRECTORY "${OXRSYS_ALVR_SOURCE_DIR}"
    COMMENT "Building alvr_server_core (${OXRSYS_ALVR_RUST_TARGET}) via cargo"
    VERBATIM
)

add_dependencies(oxrsys_runtime oxrsys_alvr_server_core)
target_link_libraries(oxrsys_runtime PRIVATE "${OXRSYS_ALVR_STAGED_DYLIB}")
target_include_directories(oxrsys_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/alvr")
target_compile_definitions(oxrsys_runtime PRIVATE OXRSYS_HAS_ALVR)

message(STATUS "ALVR backend enabled: ${OXRSYS_ALVR_RUST_TARGET} via ${OXRSYS_CARGO_BIN_DIR}")
