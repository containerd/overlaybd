find_path(GFLAGS_INCLUDE_DIRS gflags/gflags.h)

find_library(GFLAGS_LIBRARIES gflags)

find_package_handle_standard_args(gflags DEFAULT_MSG GFLAGS_LIBRARIES
                                  GFLAGS_INCLUDE_DIRS)

if(gflags_FOUND AND NOT TARGET GFLAGS::gflags)
    add_library(GFLAGS::gflags UNKNOWN IMPORTED)
    set_target_properties(
        GFLAGS::gflags
        PROPERTIES IMPORTED_LOCATION "${GFLAGS_LIBRARIES}"
                   INTERFACE_INCLUDE_DIRECTORIES "${GFLAGS_INCLUDE_DIRS}")
endif()

mark_as_advanced(GFLAGS_INCLUDE_DIRS GFLAGS_LIBRARIES)
