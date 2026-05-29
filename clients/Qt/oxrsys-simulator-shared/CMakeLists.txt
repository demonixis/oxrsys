# SPDX-License-Identifier: MPL-2.0

add_library(oxrsys-simulator-shared STATIC
    src/SimulatorWidget.cpp
)

target_include_directories(oxrsys-simulator-shared PUBLIC
    src
    ${OXRSYS_PROTOCOL_INCLUDE_DIR}
)

target_link_libraries(oxrsys-simulator-shared PUBLIC
    Qt6::Widgets
    Qt6::Network
)
