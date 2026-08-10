cmake_minimum_required(VERSION 3.16)

project(AABinaryStoreByteRange VERSION 0.1.0 LANGUAGES CXX)

option(AASTORE_BUILD_TESTS "Build AAStore tests" ON)

include(CheckIPOSupported)
check_ipo_supported(RESULT AASTORE_IPO_SUPPORTED OUTPUT AASTORE_IPO_ERROR)
find_package(Threads REQUIRED)

add_library(aastore
    src/builder.cpp
    src/byte_range_planner.cpp
    src/codec.cpp
    src/metadata.cpp
    src/store.cpp
)
target_compile_features(aastore PUBLIC cxx_std_17)
target_include_directories(aastore
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(aastore PUBLIC Threads::Threads)
target_compile_options(aastore PRIVATE -O3 -march=native -Wall -Wextra -Wpedantic)
if(AASTORE_IPO_SUPPORTED)
    set_property(TARGET aastore PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

add_executable(aastore_bench benchmarks/read_benchmark.cpp)
target_link_libraries(aastore_bench PRIVATE aastore)

if(AASTORE_BUILD_TESTS)
    enable_testing()
    add_executable(aastore_test tests/store_test.cpp)
    target_link_libraries(aastore_test PRIVATE aastore)
    add_test(NAME aastore_test COMMAND aastore_test)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(aastore PUBLIC stdc++fs)
endif()
