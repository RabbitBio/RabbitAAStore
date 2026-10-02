cmake_minimum_required(VERSION 3.16)

foreach(required AASTORE_BUILD_DIR AASTORE_SOURCE_DIR AASTORE_TEST_TOOLCHAIN_CACHE
                 AASTORE_TEST_GENERATOR AASTORE_TEST_CTEST AASTORE_TEST_PACKAGE_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${required}")
    endif()
endforeach()

if(NOT EXISTS "${AASTORE_TEST_TOOLCHAIN_CACHE}")
    message(FATAL_ERROR "Missing SDK test toolchain cache: ${AASTORE_TEST_TOOLCHAIN_CACHE}")
endif()
include("${AASTORE_SOURCE_DIR}/tests/install_consumer/toolchain_cache.cmake")
include("${AASTORE_TEST_TOOLCHAIN_CACHE}")

function(run_checked)
    execute_process(COMMAND ${ARGV}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        string(JOIN " " command ${ARGV})
        message(FATAL_ERROR "${command}\n${output}\n${error}")
    endif()
endfunction()

set(test_root "${AASTORE_BUILD_DIR}/install-consumer-${AASTORE_TEST_CONFIG}")
set(stage "${test_root}/stage")
set(relocated "${test_root}/relocated-sdk")
set(consumer_build "${test_root}/consumer-build")
set(consumer_cache "${test_root}/consumer-cache.cmake")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")
configure_file("${AASTORE_TEST_TOOLCHAIN_CACHE}" "${consumer_cache}" COPYONLY)
set(consumer_prefix_path "${relocated}")
list(APPEND consumer_prefix_path ${CMAKE_PREFIX_PATH})
aastore_test_cache_set("${consumer_cache}" CMAKE_PREFIX_PATH "${consumer_prefix_path}")
aastore_test_cache_set("${consumer_cache}" aastore_DIR
    "${relocated}/${AASTORE_TEST_PACKAGE_DIR}")
aastore_test_cache_set("${consumer_cache}" CMAKE_BUILD_TYPE "${AASTORE_TEST_CONFIG}")

set(config_args)
if(NOT "${AASTORE_TEST_CONFIG}" STREQUAL "")
    list(APPEND config_args --config "${AASTORE_TEST_CONFIG}")
endif()
run_checked("${CMAKE_COMMAND}" --install "${AASTORE_BUILD_DIR}"
    --prefix "${stage}" --component Development ${config_args})
file(RENAME "${stage}" "${relocated}")

# Exported metadata must not retain source/build paths or the original prefix.
# System dependencies are discovered when the external consumer configures.
file(GLOB_RECURSE metadata "${relocated}/*.cmake" "${relocated}/*.pc")
foreach(path IN LISTS metadata)
    file(READ "${path}" contents)
    foreach(forbidden "${AASTORE_SOURCE_DIR}" "${AASTORE_BUILD_DIR}" "${stage}")
        string(FIND "${contents}" "${forbidden}" found)
        if(NOT found EQUAL -1)
            message(FATAL_ERROR "${path} retains non-relocatable path ${forbidden}")
        endif()
    endforeach()
endforeach()

set(generator_args -G "${AASTORE_TEST_GENERATOR}")
if(NOT "${AASTORE_TEST_GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND generator_args -A "${AASTORE_TEST_GENERATOR_PLATFORM}")
endif()
if(NOT "${AASTORE_TEST_GENERATOR_TOOLSET}" STREQUAL "")
    list(APPEND generator_args -T "${AASTORE_TEST_GENERATOR_TOOLSET}")
endif()
# Exercise the SDK with its selected dependency, including custom old liburing
# builds. These test-only hints are never embedded in installed SDK metadata.
if(DEFINED AASTORE_TEST_URING_INCLUDE_DIR
   AND NOT "${AASTORE_TEST_URING_INCLUDE_DIR}" STREQUAL ""
   AND DEFINED AASTORE_TEST_URING_LIBRARY
   AND NOT "${AASTORE_TEST_URING_LIBRARY}" STREQUAL "")
    aastore_test_cache_set("${consumer_cache}" AAStoreUring_INCLUDE_DIR
        "${AASTORE_TEST_URING_INCLUDE_DIR}")
    aastore_test_cache_set("${consumer_cache}" AAStoreUring_LIBRARY
        "${AASTORE_TEST_URING_LIBRARY}")
endif()
run_checked("${CMAKE_COMMAND}"
    -C "${consumer_cache}"
    -S "${AASTORE_SOURCE_DIR}/tests/install_consumer" -B "${consumer_build}"
    ${generator_args})
run_checked("${CMAKE_COMMAND}" --build "${consumer_build}" ${config_args}
    --parallel 2)

if(AASTORE_TEST_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    message(STATUS
        "Relocated installed SDK consumer configure/build passed; runtime test skipped (no cross-compiling emulator)")
    return()
endif()

set(ctest_config_args)
if(NOT "${AASTORE_TEST_CONFIG}" STREQUAL "")
    list(APPEND ctest_config_args -C "${AASTORE_TEST_CONFIG}")
endif()
execute_process(COMMAND "${AASTORE_TEST_CTEST}" ${ctest_config_args}
    --output-on-failure WORKING_DIRECTORY "${consumer_build}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed consumer failed\n${output}\n${error}")
endif()
message(STATUS "Relocated installed SDK consumer passed")
