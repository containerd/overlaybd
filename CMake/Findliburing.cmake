# liburing (hard dependency of ublksrv).
# Distro liburing packages are commonly missing or too old (>= 2.2 required)
# and behavior differs across versions, so like every other dependency of
# this project it is always fetched at a pinned version and statically
# linked -- never taken from the system.
# FetchContent only downloads; the build uses liburing's own configure/make.
include(FetchContent)
set(FETCHCONTENT_QUIET false)

# CMake 3.30+ deprecates single-argument FetchContent_Populate (CMP0169);
# explicitly keep the old behavior
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

FetchContent_Declare(
  liburing
  GIT_REPOSITORY https://github.com/axboe/liburing.git
  GIT_TAG        liburing-2.8
)

# download only, no add_subdirectory (liburing is a plain Makefile project)
FetchContent_GetProperties(liburing)
if(NOT liburing_POPULATED)
  FetchContent_Populate(liburing)
endif()

if(NOT TARGET liburing_build)
  # OUTPUT points at the final artifact => incremental build, skipped once present
  add_custom_command(
    OUTPUT ${liburing_SOURCE_DIR}/src/liburing.a
    WORKING_DIRECTORY ${liburing_SOURCE_DIR}
    COMMAND ./configure
    COMMAND make -C src -j
    COMMENT "Building liburing with its own configure/make"
    VERBATIM
  )
  add_custom_target(liburing_build DEPENDS ${liburing_SOURCE_DIR}/src/liburing.a)
endif()

set(LIBURING_LIBRARIES    ${liburing_SOURCE_DIR}/src/liburing.a)
set(LIBURING_INCLUDE_DIRS ${liburing_SOURCE_DIR}/src/include)

# Generate an in-build-tree liburing.pc for ublksrv's configure check
# PKG_CHECK_MODULES([LIBURING], [liburing >= 2.2]) (the official
# build_with_liburing_src flow relies on pkg-config metadata)
set(LIBURING_PC_DIR ${CMAKE_BINARY_DIR}/liburing-pkgconfig)
file(WRITE ${LIBURING_PC_DIR}/liburing.pc
"prefix=${liburing_SOURCE_DIR}
libdir=${liburing_SOURCE_DIR}/src
includedir=${liburing_SOURCE_DIR}/src/include

Name: liburing
Version: 2.8
Description: io_uring library
Libs: -L\${libdir} -luring
Cflags: -I\${includedir}
")

if(NOT TARGET liburing_static)
  add_library(liburing_static STATIC IMPORTED GLOBAL)
  set_target_properties(liburing_static PROPERTIES
    IMPORTED_LOCATION             ${LIBURING_LIBRARIES}
    INTERFACE_INCLUDE_DIRECTORIES ${LIBURING_INCLUDE_DIRS}
  )
endif()
add_dependencies(liburing_static liburing_build)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(liburing DEFAULT_MSG LIBURING_LIBRARIES LIBURING_INCLUDE_DIRS)

mark_as_advanced(LIBURING_LIBRARIES LIBURING_INCLUDE_DIRS)
