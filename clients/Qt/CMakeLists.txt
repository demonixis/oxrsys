# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(OXRSysQt LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    include(cmake/DetectQt6.cmake)
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(NOT DEFINED OXRSYS_PROTOCOL_INCLUDE_DIR)
    get_filename_component(OXRSYS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(OXRSYS_PROTOCOL_INCLUDE_DIR "${OXRSYS_REPO_ROOT}/common/protocol/include")
endif()

add_subdirectory(oxrsys-simulator-shared)
add_subdirectory(oxrsys-simulator)
add_subdirectory(oxrsys-home)
