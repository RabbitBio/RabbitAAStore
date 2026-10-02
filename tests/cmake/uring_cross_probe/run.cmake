cmake_minimum_required(VERSION 3.16)

if(NOT AASTORE_TEST_BINARY_DIR)
    message(FATAL_ERROR "AASTORE_TEST_BINARY_DIR is required")
endif()
if(NOT AASTORE_TEST_MODULE_DIR)
    get_filename_component(AASTORE_TEST_MODULE_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../../../cmake" ABSOLUTE)
endif()
file(MAKE_DIRECTORY "${AASTORE_TEST_BINARY_DIR}")
set(common_arguments)
if(AASTORE_TEST_CXX_COMPILER)
    list(APPEND common_arguments "-DCMAKE_CXX_COMPILER=${AASTORE_TEST_CXX_COMPILER}")
endif()
if(DEFINED AASTORE_TEST_CXX_FLAGS)
    list(APPEND common_arguments "-DCMAKE_CXX_FLAGS=${AASTORE_TEST_CXX_FLAGS}")
endif()
if(AASTORE_TEST_GENERATOR)
    list(APPEND common_arguments -G "${AASTORE_TEST_GENERATOR}")
endif()

function(run_command)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Command failed (${result}): ${ARGV}\n${output}\n${error}")
    endif()
endfunction()

set(library_build "${AASTORE_TEST_BINARY_DIR}/libraries")
run_command("${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/libraries"
    -B "${library_build}" ${common_arguments} -DCMAKE_BUILD_TYPE=Release)
run_command("${CMAKE_COMMAND}" --build "${library_build}" --config Release)

# Setting CMAKE_SYSTEM_NAME marks even a same-architecture compiler as cross.
# This exercises CMake's cross-compiling behavior without a platform toolchain.
set(toolchain "${AASTORE_TEST_BINARY_DIR}/cross-toolchain.cmake")
file(WRITE "${toolchain}"
    "set(CMAKE_SYSTEM_NAME Linux)\nset(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n")
foreach(caller_type IN ITEMS STATIC_LIBRARY UNDEFINED)
    foreach(library IN ITEMS empty baseline complete)
        set(expected_found TRUE)
        set(expected_optional FALSE)
        if(library STREQUAL "empty")
            set(expected_found FALSE)
        elseif(library STREQUAL "complete")
            set(expected_optional TRUE)
        endif()
        run_command("${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/probe"
            -B "${AASTORE_TEST_BINARY_DIR}/${library}-${caller_type}"
            ${common_arguments} "-DCMAKE_TOOLCHAIN_FILE=${toolchain}"
            "-DAASTORE_TEST_MODULE_DIR=${AASTORE_TEST_MODULE_DIR}"
            "-DAASTORE_TEST_INCLUDE_DIR=${CMAKE_CURRENT_LIST_DIR}"
            "-DAASTORE_TEST_LIBRARY=${library_build}/lib/lib${library}.a"
            "-DAASTORE_TEST_CALLER_TRY_TYPE=${caller_type}"
            "-DAASTORE_TEST_EXPECT_FOUND=${expected_found}"
            "-DAASTORE_TEST_EXPECT_OPTIONAL=${expected_optional}")
    endforeach()
endforeach()
message(STATUS "liburing cross-compilation checks passed for all six cases")
