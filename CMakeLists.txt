cmake_minimum_required(VERSION 3.10)
project(sdsl_protein LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 可选：Release 优化
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-O3 -march=native)

# 查找线程库
find_package(Threads REQUIRED)
find_package(OpenMP)
find_path(LIBURING_INCLUDE_DIR liburing.h)
find_library(LIBURING_LIBRARY uring)

add_executable(aatools src/AAStoreTools.cpp)
add_executable(kmercount src/kmercount.cpp)
add_executable(benchmark src/benchmark.cpp)
add_executable(kseq_benchmark src/kseq_benchmark.cpp)
add_executable(meta_index_benchmark src/meta_index_benchmark.cpp)
add_executable(paged_aatools src/PagedAAStoreTools.cpp)

# 链接线程库
target_link_libraries(aatools PRIVATE Threads::Threads)
target_link_libraries(kmercount PRIVATE Threads::Threads)
target_link_libraries(paged_aatools PRIVATE Threads::Threads)
if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
  target_compile_definitions(paged_aatools PRIVATE PAGED_AASTORE_ENABLE_IO_URING)
  target_include_directories(paged_aatools PRIVATE ${LIBURING_INCLUDE_DIR})
  target_link_libraries(paged_aatools PRIVATE ${LIBURING_LIBRARY})
endif()
if(OpenMP_CXX_FOUND)
  target_link_libraries(aatools PRIVATE OpenMP::OpenMP_CXX)
  target_link_libraries(kmercount PRIVATE OpenMP::OpenMP_CXX)
endif()

# 如果使用 GCC 8 可能需要显式链接 stdc++fs
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(aatools PRIVATE stdc++fs)
    target_link_libraries(kmercount PRIVATE stdc++fs)
    target_link_libraries(benchmark PRIVATE stdc++fs)
    target_link_libraries(paged_aatools PRIVATE stdc++fs)
  endif()
endif()


# 如果 sdsl 在本仓库相对路径（例如 src/sdsl），可加入包含目录
target_include_directories(aatools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(kmercount PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(kseq_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(meta_index_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(paged_aatools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
