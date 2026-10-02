cmake_minimum_required(VERSION 3.16)
project(AAStoreUringCrossProbe LANGUAGES CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "The regression must configure a cross-compiling project")
endif()
if(AASTORE_TEST_CALLER_TRY_TYPE STREQUAL "UNDEFINED")
    unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
else()
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${AASTORE_TEST_CALLER_TRY_TYPE}")
endif()

set(CMAKE_REQUIRED_FLAGS "-DAASTORE_PARENT_CHECK_FLAG")
set(CMAKE_REQUIRED_DEFINITIONS "-DAASTORE_PARENT_CHECK_DEFINITION")
set(CMAKE_REQUIRED_INCLUDES "/aastore/missing/parent/check/include")
set(CMAKE_REQUIRED_LINK_OPTIONS "-Wl,--aastore-invalid-parent-check-option")
set(CMAKE_REQUIRED_LIBRARIES aastore_missing_parent_check_library)
set(CMAKE_REQUIRED_QUIET FALSE)
set(CMAKE_EXTRA_INCLUDE_FILES aastore_missing_parent_check_header.h)
foreach(variable IN ITEMS CMAKE_REQUIRED_FLAGS CMAKE_REQUIRED_DEFINITIONS
        CMAKE_REQUIRED_INCLUDES CMAKE_REQUIRED_LINK_OPTIONS
        CMAKE_REQUIRED_LIBRARIES CMAKE_REQUIRED_QUIET CMAKE_EXTRA_INCLUDE_FILES)
    set("original_${variable}" "${${variable}}")
endforeach()

set(AAStoreUring_INCLUDE_DIR "${AASTORE_TEST_INCLUDE_DIR}" CACHE PATH "" FORCE)
set(AAStoreUring_LIBRARY "${AASTORE_TEST_LIBRARY}" CACHE FILEPATH "" FORCE)
# Recreate an old configure cache whose compile-only checks all succeeded.
# Its file fingerprint is correct, so file-change invalidation cannot mask a
# missing migration/recheck of these cached API capabilities.
file(SHA256 "${AAStoreUring_INCLUDE_DIR}/liburing.h" header_hash)
file(SHA256 "${AAStoreUring_LIBRARY}" library_hash)
set(AAStoreUring_CAPABILITY_FILES
    "${AAStoreUring_INCLUDE_DIR}|${AAStoreUring_LIBRARY}|${header_hash}|${library_hash}"
    CACHE INTERNAL "" FORCE)
foreach(capability IN ITEMS BASELINE READ PROBE)
    set("AAStoreUring_HAS_${capability}" TRUE CACHE INTERNAL "" FORCE)
endforeach()

include("${AASTORE_TEST_MODULE_DIR}/FindAAStoreUring.cmake")

if(AASTORE_TEST_EXPECT_FOUND)
    if(NOT AAStoreUring_FOUND)
        message(FATAL_ERROR "A usable liburing library was rejected")
    endif()
    if(NOT AAStoreUring_ABI MATCHES "^[0-9]+(:[0-9]+)+$")
        message(FATAL_ERROR "Cross-compilation did not extract the ABI layout")
    endif()
else()
    if(AAStoreUring_FOUND OR AAStoreUring_HAS_BASELINE)
        message(FATAL_ERROR "An archive without liburing symbols was accepted")
    endif()
endif()
foreach(capability IN ITEMS READ PROBE)
    if(AASTORE_TEST_EXPECT_OPTIONAL)
        if(NOT AAStoreUring_HAS_${capability})
            message(FATAL_ERROR "A linked ${capability} API was rejected")
        endif()
    elseif(AAStoreUring_HAS_${capability})
        message(FATAL_ERROR "An unlinked ${capability} API was accepted")
    endif()
endforeach()

if(AASTORE_TEST_CALLER_TRY_TYPE STREQUAL "UNDEFINED")
    if(DEFINED CMAKE_TRY_COMPILE_TARGET_TYPE)
        message(FATAL_ERROR "The caller's unset try_compile target type changed")
    endif()
elseif(NOT CMAKE_TRY_COMPILE_TARGET_TYPE STREQUAL AASTORE_TEST_CALLER_TRY_TYPE)
    message(FATAL_ERROR "The caller's try_compile target type changed")
endif()
foreach(variable IN ITEMS CMAKE_REQUIRED_FLAGS CMAKE_REQUIRED_DEFINITIONS
        CMAKE_REQUIRED_INCLUDES CMAKE_REQUIRED_LINK_OPTIONS
        CMAKE_REQUIRED_LIBRARIES CMAKE_REQUIRED_QUIET CMAKE_EXTRA_INCLUDE_FILES)
    if(NOT "${${variable}}" STREQUAL "${original_${variable}}")
        message(FATAL_ERROR "The caller's ${variable} changed")
    endif()
endforeach()
