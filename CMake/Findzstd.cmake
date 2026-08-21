find_path(ZSTD_INCLUDE_DIRS zstd.h)

find_library(ZSTD_LIBRARIES zstd)

find_package_handle_standard_args(zstd DEFAULT_MSG ZSTD_LIBRARIES
                                  ZSTD_INCLUDE_DIRS)

if(zstd_FOUND AND NOT TARGET ZSTD::zstd)
    add_library(ZSTD::zstd UNKNOWN IMPORTED)
    set_target_properties(
        ZSTD::zstd
        PROPERTIES IMPORTED_LINK_INTERFACE_LANGUAGES "C"
                   IMPORTED_LOCATION "${ZSTD_LIBRARIES}"
                   INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIRS}")
endif()

mark_as_advanced(ZSTD_INCLUDE_DIRS ZSTD_LIBRARIES)
