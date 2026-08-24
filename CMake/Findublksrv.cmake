# ublksrv (ublk userspace library; autotools project, no CMake).
# Only the lib/ directory (pure-C libublksrv) is built:
#   - avoids the C++20/-fcoroutines requirement of the ublk CLI tool;
#   - libnfs/libiscsi/gnutls only serve its bundled targets, all disabled.
# liburing is built from source by Findliburing.cmake and fed to configure
# via pkg-config/CFLAGS/LDFLAGS (the official build_with_liburing_src flow).
# License: lib/ + include/ublksrv.h are dual MIT/LGPL; linked under MIT.
include(FetchContent)
set(FETCHCONTENT_QUIET false)

if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

# the autotools build needs extra host tools; fail early with a clear message
find_program(AUTORECONF_EXECUTABLE autoreconf)
find_program(LIBTOOLIZE_EXECUTABLE libtoolize)
find_program(AUTOMAKE_EXECUTABLE automake)
if(NOT AUTORECONF_EXECUTABLE OR NOT LIBTOOLIZE_EXECUTABLE OR NOT AUTOMAKE_EXECUTABLE)
  message(FATAL_ERROR
    "BUILD_UBLK_FRONTEND requires autotools (autoconf/automake/libtool) "
    "to build ublksrv. Install them or configure with -DBUILD_UBLK_FRONTEND=off")
endif()

FetchContent_Declare(
  ublksrv
  GIT_REPOSITORY https://github.com/ublk-org/ublksrv.git
  GIT_TAG        f6c643952d1cdc7f6460630638fe6b5454ca1c4d   # v1.7
)

FetchContent_GetProperties(ublksrv)
if(NOT ublksrv_POPULATED)
  FetchContent_Populate(ublksrv)
endif()

if(NOT TARGET libublksrv_build)
  add_custom_command(
    OUTPUT ${ublksrv_SOURCE_DIR}/lib/.libs/libublksrv.a
    WORKING_DIRECTORY ${ublksrv_SOURCE_DIR}
    COMMAND autoreconf -i
    COMMAND ${CMAKE_COMMAND} -E env PKG_CONFIG_PATH=${LIBURING_PC_DIR}
            ./configure
            --without-libnfs --without-libiscsi --without-gnutls
            # configure appends -fcoroutines whenever $CXX matches *g++*
            # (GCC 10+ only, and only the ublk tool needs it, lib/ does not);
            # use the name c++ to sidestep that match
            CXX=c++
            "CFLAGS=-I${LIBURING_INCLUDE_DIRS} -O2"
            "CXXFLAGS=-I${LIBURING_INCLUDE_DIRS} -O2"
            "LDFLAGS=-L${liburing_SOURCE_DIR}/src"
    COMMAND make -C lib -j
    DEPENDS ${LIBURING_LIBRARIES}
    COMMENT "Building libublksrv (lib/ only) with autotools"
    VERBATIM
  )
  add_custom_target(libublksrv_build DEPENDS ${ublksrv_SOURCE_DIR}/lib/.libs/libublksrv.a)
  add_dependencies(libublksrv_build liburing_build)
endif()

set(UBLKSRV_LIBRARIES    ${ublksrv_SOURCE_DIR}/lib/.libs/libublksrv.a)
set(UBLKSRV_INCLUDE_DIRS ${ublksrv_SOURCE_DIR}/include)

if(NOT TARGET libublksrv_static)
  add_library(libublksrv_static STATIC IMPORTED GLOBAL)
  set_target_properties(libublksrv_static PROPERTIES
    IMPORTED_LOCATION             ${UBLKSRV_LIBRARIES}
    INTERFACE_INCLUDE_DIRECTORIES ${UBLKSRV_INCLUDE_DIRS}
    # libublksrv.a references liburing symbols; propagate via INTERFACE so
    # consumers get the correct link order automatically
    INTERFACE_LINK_LIBRARIES      liburing_static
  )
endif()
add_dependencies(libublksrv_static libublksrv_build)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ublksrv DEFAULT_MSG UBLKSRV_LIBRARIES UBLKSRV_INCLUDE_DIRS)

mark_as_advanced(UBLKSRV_LIBRARIES UBLKSRV_INCLUDE_DIRS)
