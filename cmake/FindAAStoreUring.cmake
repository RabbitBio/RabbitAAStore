# Find liburing on the current machine, never in the SDK producer's directories.
find_path(AAStoreUring_INCLUDE_DIR NAMES liburing.h)
find_library(AAStoreUring_LIBRARY NAMES uring)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(AAStoreUring
    REQUIRED_VARS AAStoreUring_LIBRARY AAStoreUring_INCLUDE_DIR)
mark_as_advanced(AAStoreUring_INCLUDE_DIR AAStoreUring_LIBRARY)

if(AAStoreUring_FOUND AND NOT TARGET AAStoreUring::AAStoreUring)
    add_library(AAStoreUring::AAStoreUring UNKNOWN IMPORTED)
    set_target_properties(AAStoreUring::AAStoreUring PROPERTIES
        IMPORTED_LOCATION "${AAStoreUring_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${AAStoreUring_INCLUDE_DIR}")
endif()
