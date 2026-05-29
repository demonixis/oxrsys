# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(OXRSysSimulator LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)

    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/DetectQt6.cmake")
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

    set(OXRSYS_PROTOCOL_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../common/protocol/include")

    if(NOT TARGET oxrsys-simulator-shared)
        add_subdirectory(
            "${CMAKE_CURRENT_LIST_DIR}/../oxrsys-simulator-shared"
            "${CMAKE_CURRENT_BINARY_DIR}/oxrsys-simulator-shared"
        )
    endif()
endif()

add_executable(oxrsys-simulator
    src/main.cpp
)

target_include_directories(oxrsys-simulator PRIVATE
    src
)

target_link_libraries(oxrsys-simulator PRIVATE
    oxrsys-simulator-shared
)
