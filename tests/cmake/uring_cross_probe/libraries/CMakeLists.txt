cmake_minimum_required(VERSION 3.16)
project(AAStoreUringProbeLibraries LANGUAGES CXX)

add_library(empty STATIC empty.cpp)
add_library(baseline STATIC liburing.cpp)
add_library(complete STATIC liburing.cpp)
target_compile_definitions(complete PRIVATE AASTORE_TEST_COMPLETE_LIBURING)
target_include_directories(baseline PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(complete PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_target_properties(empty baseline complete PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(empty baseline complete PROPERTIES
        "ARCHIVE_OUTPUT_DIRECTORY_${config}" "${CMAKE_CURRENT_BINARY_DIR}/lib")
endforeach()
