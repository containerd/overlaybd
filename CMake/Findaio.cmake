find_path(AIO_INCLUDE_DIR libaio.h)

find_library(AIO_LIBRARIES aio)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(aio DEFAULT_MSG AIO_LIBRARIES AIO_INCLUDE_DIR)

if(AIO_FOUND AND NOT TARGET AIO::aio)
  add_library(AIO::aio UNKNOWN IMPORTED)
  set_target_properties(
    AIO::aio
    PROPERTIES IMPORTED_LINK_INTERFACE_LANGUAGES "C"
               IMPORTED_LOCATION "${AIO_LIBRARIES}"
               INTERFACE_INCLUDE_DIRECTORIES "${AIO_INCLUDE_DIR}")
endif()

mark_as_advanced(AIO_INCLUDE_DIR AIO_LIBRARIES)