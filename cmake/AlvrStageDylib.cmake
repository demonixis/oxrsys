# SPDX-License-Identifier: MPL-2.0
# Stages libalvr_server_core.dylib next to liboxrsys-runtime.dylib with an
# @loader_path install name + ad-hoc signature. Skips when the cargo output
# is unchanged so the runtime is not gratuitously relinked every build.
# Args: -DBUILT=<cargo dylib> -DSTAGED=<destination dylib>

if(NOT EXISTS "${BUILT}")
    message(FATAL_ERROR "cargo output missing: ${BUILT}")
endif()

if(EXISTS "${STAGED}" AND NOT "${BUILT}" IS_NEWER_THAN "${STAGED}")
    return()
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E copy "${BUILT}" "${STAGED}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(
    COMMAND install_name_tool -id "@loader_path/libalvr_server_core.dylib" "${STAGED}"
    COMMAND_ERROR_IS_FATAL ANY
)
execute_process(COMMAND codesign --force --sign - "${STAGED}" COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "Staged libalvr_server_core.dylib -> ${STAGED}")
