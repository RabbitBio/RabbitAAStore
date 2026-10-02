# Keep toolchain settings in a test-only cache file instead of command-line
# lists, which would split emulator arguments and search paths at semicolons.
function(aastore_test_cache_set path name value)
    set(delimiter "=")
    string(FIND "${value}" "]${delimiter}]" closing)
    while(NOT closing EQUAL -1)
        string(APPEND delimiter "=")
        string(FIND "${value}" "]${delimiter}]" closing)
    endwhile()
    file(APPEND "${path}"
        "set(${name} [${delimiter}[${value}]${delimiter}] CACHE STRING \"AAStore SDK consumer test\" FORCE)\n")
endfunction()

function(aastore_write_consumer_toolchain_cache path)
    file(WRITE "${path}" "# Toolchain settings for the relocated SDK test only.\n")
    set(variables
        CMAKE_TOOLCHAIN_FILE CMAKE_MAKE_PROGRAM
        CMAKE_CXX_COMPILER CMAKE_CXX_COMPILER_ARG1
        CMAKE_CXX_COMPILER_TARGET CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN
        CMAKE_CXX_COMPILER_LAUNCHER CMAKE_AR CMAKE_RANLIB CMAKE_LINKER
        CMAKE_SYSROOT CMAKE_SYSROOT_COMPILE CMAKE_SYSROOT_LINK
        CMAKE_TRY_COMPILE_TARGET_TYPE CMAKE_CROSSCOMPILING_EMULATOR
        CMAKE_CXX_STANDARD CMAKE_CXX_STANDARD_REQUIRED CMAKE_CXX_EXTENSIONS
        CMAKE_CONFIGURATION_TYPES CMAKE_PREFIX_PATH CMAKE_FIND_ROOT_PATH
        CMAKE_FIND_ROOT_PATH_MODE_PROGRAM CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
        CMAKE_FIND_ROOT_PATH_MODE_INCLUDE CMAKE_FIND_ROOT_PATH_MODE_PACKAGE
        CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
        CMAKE_MODULE_LINKER_FLAGS CMAKE_STATIC_LINKER_FLAGS)
    # Setting SYSTEM_NAME on a native consumer would turn it into a cross build.
    if(CMAKE_CROSSCOMPILING)
        list(APPEND variables
            CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION CMAKE_SYSTEM_PROCESSOR)
    endif()
    set(configurations DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    list(APPEND configurations ${CMAKE_CONFIGURATION_TYPES} "${CMAKE_BUILD_TYPE}")
    list(REMOVE_DUPLICATES configurations)
    foreach(configuration IN LISTS configurations)
        if(NOT "${configuration}" STREQUAL "")
            string(TOUPPER "${configuration}" configuration)
            foreach(kind CXX EXE_LINKER SHARED_LINKER MODULE_LINKER STATIC_LINKER)
                list(APPEND variables "CMAKE_${kind}_FLAGS_${configuration}")
            endforeach()
        endif()
    endforeach()
    foreach(variable IN LISTS variables)
        if(DEFINED ${variable})
            set(value "${${variable}}")
            if(variable STREQUAL "CMAKE_TOOLCHAIN_FILE" AND NOT "${value}" STREQUAL "")
                # Match CMake's relative-toolchain search order before moving
                # to the consumer's independent source and build directories.
                if(NOT IS_ABSOLUTE "${value}")
                    if(EXISTS "${CMAKE_BINARY_DIR}/${value}")
                        set(value "${CMAKE_BINARY_DIR}/${value}")
                    else()
                        set(value "${CMAKE_SOURCE_DIR}/${value}")
                    endif()
                endif()
                get_filename_component(value "${value}" ABSOLUTE)
                if(NOT EXISTS "${value}")
                    message(FATAL_ERROR "SDK test toolchain file does not exist: ${value}")
                endif()
            endif()
            aastore_test_cache_set("${path}" "${variable}" "${value}")
        endif()
    endforeach()
endfunction()
