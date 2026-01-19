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

add_executable(test src/test.cpp)
add_executable(kmercount src/kmercount.cpp)

# 链接线程库
target_link_libraries(kmercount PRIVATE Threads::Threads)
if(OpenMP_CXX_FOUND)
  target_link_libraries(test PRIVATE OpenMP::OpenMP_CXX)
  target_link_libraries(kmercount PRIVATE OpenMP::OpenMP_CXX)
endif()

# 如果使用 GCC 8 可能需要显式链接 stdc++fs
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(test PRIVATE stdc++fs)
    target_link_libraries(kmercount PRIVATE stdc++fs)
  endif()
endif()


# 如果 sdsl 在本仓库相对路径（例如 src/sdsl），可加入包含目录
target_include_directories(test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(kmercount PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
