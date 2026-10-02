# Build and install the C++ SDK, optional tools, benchmarks, and Python bindings.
cmake_minimum_required(VERSION 3.16)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AASTORE_VERSION LIMIT_COUNT 1)
string(STRIP "${AASTORE_VERSION}" AASTORE_VERSION)
if(NOT AASTORE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a numeric major.minor.patch version")
endif()
project(AABinaryStore VERSION ${AASTORE_VERSION} LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(AASTORE_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(AASTORE_IS_TOP_LEVEL ON)
    option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
endif()
option(AASTORE_BUILD_TESTS "Build AAStore tests" ${AASTORE_IS_TOP_LEVEL})
option(AASTORE_BUILD_TOOLS "Build AAStore command-line tools" ${AASTORE_IS_TOP_LEVEL})
option(AASTORE_BUILD_BENCHMARKS "Build AAStore benchmarks" ${AASTORE_IS_TOP_LEVEL})
option(AASTORE_BUILD_PYTHON "Build the Python bindings" OFF)
option(AASTORE_ENABLE_IO_URING "Enable the Linux io_uring read backend" ON)

set(AASTORE_IPO_DEFAULT ${AASTORE_IS_TOP_LEVEL})
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    set(AASTORE_IPO_DEFAULT OFF)
endif()
option(AASTORE_ENABLE_IPO "Enable interprocedural optimization when supported"
       ${AASTORE_IPO_DEFAULT})
if(AASTORE_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT AASTORE_IPO_SUPPORTED OUTPUT AASTORE_IPO_ERROR
                        LANGUAGES CXX)
endif()

find_package(Threads REQUIRED)
set(AASTORE_HAVE_IO_URING OFF)
if(AASTORE_ENABLE_IO_URING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_aastore_module_path "${CMAKE_MODULE_PATH}")
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(AAStoreUring QUIET)
    set(CMAKE_MODULE_PATH "${_aastore_module_path}")
    unset(_aastore_module_path)
    if(AAStoreUring_FOUND)
        set(AASTORE_HAVE_IO_URING ON)
    else()
        message(WARNING "liburing not found; using the pread backend")
    endif()
endif()

add_library(aastore
    src/builder.cpp
    src/builder_file.cpp
    src/builder_sequence_writer.cpp
    src/builder_text_writer.cpp
    src/byte_range_planner.cpp
    src/codec.cpp
    src/fasta_reader.cpp
    src/io_uring_backend.cpp
    src/metadata.cpp
    src/store_batch.cpp
    src/store.cpp
    src/store_io.cpp
    src/store_sequential.cpp
    src/text_column.cpp
)
add_library(aastore::aastore ALIAS aastore)
target_compile_features(aastore PUBLIC cxx_std_17)
target_include_directories(aastore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(aastore PUBLIC Threads::Threads)
set_target_properties(aastore PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
)
if(PROJECT_VERSION_MAJOR EQUAL 0)
    # Each pre-1.0 minor may change the ABI; patches preserve it.
    set_target_properties(aastore PROPERTIES
        SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
    set(AASTORE_VERSION_COMPATIBILITY SameMinorVersion)
else()
    set_target_properties(aastore PROPERTIES SOVERSION ${PROJECT_VERSION_MAJOR})
    set(AASTORE_VERSION_COMPATIBILITY SameMajorVersion)
endif()
if(AASTORE_HAVE_IO_URING)
    target_compile_definitions(aastore PRIVATE AASTORE_HAVE_IO_URING=1)
    # The installed static target references a dependency name, not the build
    # host's absolute liburing library/include paths. Consumers rediscover it.
    target_link_libraries(aastore PRIVATE AAStoreUring::AAStoreUring)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aastore PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(AASTORE_ENABLE_IPO AND AASTORE_IPO_SUPPORTED)
    set_property(TARGET aastore PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

set(AASTORE_PKGCONFIG_PRIVATE_LIBS "${CMAKE_THREAD_LIBS_INIT}")
set(AASTORE_PKGCONFIG_PUBLIC_LIBS "")
if(AASTORE_HAVE_IO_URING)
    string(APPEND AASTORE_PKGCONFIG_PRIVATE_LIBS " -luring")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(aastore PUBLIC stdc++fs)
    set(AASTORE_PKGCONFIG_PUBLIC_LIBS "-lstdc++fs")
endif()
string(STRIP "${AASTORE_PKGCONFIG_PRIVATE_LIBS}" AASTORE_PKGCONFIG_PRIVATE_LIBS)

set(AASTORE_PACKAGE_NEEDS_URING OFF)
get_target_property(_aastore_library_type aastore TYPE)
if(AASTORE_HAVE_IO_URING AND _aastore_library_type STREQUAL "STATIC_LIBRARY")
    set(AASTORE_PACKAGE_NEEDS_URING ON)
endif()
unset(_aastore_library_type)

install(TARGETS aastore EXPORT aastore-targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Development
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/aastore
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT Development
    FILES_MATCHING PATTERN "*.hpp"
)
install(FILES LICENSE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/aastore"
    COMPONENT Development
)
set(AASTORE_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/aastore")
configure_package_config_file(
    cmake/aastore-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/aastore-config.cmake"
    INSTALL_DESTINATION "${AASTORE_CMAKE_INSTALL_DIR}"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/aastore-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY ${AASTORE_VERSION_COMPATIBILITY}
)
install(EXPORT aastore-targets
    FILE aastore-targets.cmake
    NAMESPACE aastore::
    DESTINATION "${AASTORE_CMAKE_INSTALL_DIR}"
    COMPONENT Development
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/aastore-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/aastore-config-version.cmake"
    cmake/FindAAStoreUring.cmake
    DESTINATION "${AASTORE_CMAKE_INSTALL_DIR}"
    COMPONENT Development
)
# Resolve prefix from pcfiledir so the SDK may be moved as a unit, including
# GNUInstallDirs configurations using lib64 or a multiarch lib directory.
file(RELATIVE_PATH AASTORE_PKGCONFIG_PREFIX_RELATIVE
    "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
file(RELATIVE_PATH AASTORE_PKGCONFIG_LIBDIR_RELATIVE
    "${CMAKE_INSTALL_PREFIX}" "${CMAKE_INSTALL_FULL_LIBDIR}")
file(RELATIVE_PATH AASTORE_PKGCONFIG_INCLUDEDIR_RELATIVE
    "${CMAKE_INSTALL_PREFIX}" "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
configure_file(cmake/aastore.pc.in "${CMAKE_CURRENT_BINARY_DIR}/aastore.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/aastore.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    COMPONENT Development
)

if(AASTORE_BUILD_PYTHON)
    set(PYBIND11_FINDPYTHON ON)
    find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
    find_package(pybind11 3.0 CONFIG REQUIRED)
    pybind11_add_module(_aastore NO_EXTRAS python/bindings.cpp)
    target_link_libraries(_aastore PRIVATE aastore)
    target_compile_definitions(_aastore PRIVATE
        AASTORE_PYTHON_VERSION="${PROJECT_VERSION}")
    set_target_properties(_aastore PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/python/aastore")
    configure_file(python/aastore/__init__.py python/aastore/__init__.py COPYONLY)
    install(TARGETS _aastore
        LIBRARY DESTINATION aastore COMPONENT Python
        RUNTIME DESTINATION aastore COMPONENT Python)
endif()

if(AASTORE_BUILD_TOOLS)
    add_executable(aastore_build tools/build_store.cpp)
    target_link_libraries(aastore_build PRIVATE aastore)
    add_executable(aastore_sort_block tools/sort_block.cpp)
    target_link_libraries(aastore_sort_block PRIVATE aastore)
    add_executable(aastore_reorder_createdb tools/reorder_createdb.cpp)
    target_include_directories(aastore_reorder_createdb PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aastore_reorder_createdb PRIVATE aastore)
    file(RELATIVE_PATH _aastore_tools_libdir
        "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(aastore_build aastore_sort_block aastore_reorder_createdb
            PROPERTIES INSTALL_RPATH "$ORIGIN/${_aastore_tools_libdir}")
    endif()
    unset(_aastore_tools_libdir)
    install(TARGETS aastore_build aastore_sort_block aastore_reorder_createdb
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime)
endif()

if(AASTORE_BUILD_BENCHMARKS)
    add_executable(aastore_bench benchmarks/read_benchmark.cpp)
    target_link_libraries(aastore_bench PRIVATE aastore)
    add_executable(linclust_candidate_bench tools/linclust_candidate_benchmark.cpp)
    target_link_libraries(linclust_candidate_bench PRIVATE aastore)
    add_executable(aastore_window_bench tools/window_read_benchmark.cpp)
    target_link_libraries(aastore_window_bench PRIVATE aastore)
endif()

if(AASTORE_BUILD_TESTS)
    enable_testing()
    if(AASTORE_BUILD_PYTHON)
        add_test(NAME aastore_python_test
            COMMAND ${CMAKE_COMMAND} -E env
                "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python"
                ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_test.py)
    endif()
    add_executable(aastore_test tests/store_test.cpp)
    target_link_libraries(aastore_test PRIVATE aastore)
    add_test(NAME aastore_test COMMAND aastore_test)
    add_executable(aastore_codec_test tests/codec_test.cpp)
    target_link_libraries(aastore_codec_test PRIVATE aastore)
    add_test(NAME aastore_codec_test COMMAND aastore_codec_test)
    if(AASTORE_HAVE_IO_URING)
        target_compile_definitions(aastore_test PRIVATE AASTORE_TEST_IO_URING=1)
        add_executable(aastore_io_uring_test tests/io_uring_test.cpp)
        target_link_libraries(aastore_io_uring_test PRIVATE aastore)
        add_test(NAME aastore_io_uring_test COMMAND aastore_io_uring_test)
        set_tests_properties(aastore_io_uring_test PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
       AND TARGET aastore_build AND TARGET aastore_bench)
        add_test(NAME aastore_benchmark_test
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_test.py
                $<TARGET_FILE:aastore_build> $<TARGET_FILE:aastore_bench>)
    endif()
    add_test(NAME aastore_install_consumer
        COMMAND ${CMAKE_COMMAND}
            "-DAASTORE_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DAASTORE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DAASTORE_TEST_CONFIG=$<CONFIG>"
            "-DAASTORE_TEST_GENERATOR=${CMAKE_GENERATOR}"
            "-DAASTORE_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DAASTORE_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DAASTORE_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DAASTORE_TEST_CTEST=${CMAKE_CTEST_COMMAND}"
            "-DAASTORE_TEST_PACKAGE_DIR=${AASTORE_CMAKE_INSTALL_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_consumer/run.cmake")
    set_tests_properties(aastore_install_consumer PROPERTIES LABELS packaging)
endif()
