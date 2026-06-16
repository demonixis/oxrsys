# SPDX-License-Identifier: MPL-2.0

if(NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "INPUT_FILE and OUTPUT_FILE must be provided.")
endif()

if(EXISTS "${INPUT_FILE}")
    if(EXISTS "${OUTPUT_FILE}" OR IS_SYMLINK "${OUTPUT_FILE}")
        file(REMOVE "${OUTPUT_FILE}")
    endif()

    if(CMAKE_HOST_WIN32)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${INPUT_FILE}" "${OUTPUT_FILE}"
            RESULT_VARIABLE copy_result
        )
        if(NOT copy_result EQUAL 0)
            message(FATAL_ERROR "Failed to sync compile_commands.json.")
        endif()
        message(STATUS "Updated compile_commands.json copy at ${OUTPUT_FILE}")
    else()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E create_symlink "${INPUT_FILE}" "${OUTPUT_FILE}"
            RESULT_VARIABLE symlink_result
        )

        if(symlink_result EQUAL 0)
            message(STATUS "Updated compile_commands.json symlink at ${OUTPUT_FILE}")
        else()
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${INPUT_FILE}" "${OUTPUT_FILE}"
                RESULT_VARIABLE copy_result
            )
            if(NOT copy_result EQUAL 0)
                message(FATAL_ERROR "Failed to sync compile_commands.json.")
            endif()
            message(STATUS "Updated compile_commands.json copy at ${OUTPUT_FILE}")
        endif()
    endif()
else()
    message(STATUS "compile_commands.json is not available for this generator yet.")
endif()
