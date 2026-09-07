cmake_minimum_required(VERSION 3.14)

project(
  overlaybd
  LANGUAGES C CXX
)
enable_language(C)

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -DNDEBUG -g")

if (NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64) AND NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64) AND NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL arm64))
    message(FATAL_ERROR "Unknown CPU architecture ${CMAKE_SYSTEM_PROCESSOR}")
endif ()

option(OBD_VER "Overlaybd version" "overlaybd/0.0.0-undefined")

set(LIBRARY_OUTPUT_PATH "${CMAKE_SOURCE_DIR}/build/output")
set(EXECUTABLE_OUTPUT_PATH "${CMAKE_SOURCE_DIR}/build/output")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fpic -Wall -Werror=sign-compare")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpic -Wall -Werror=sign-compare -DOVERLAYBD_VER=${OBD_VER}")

# GCC 13+ (default on Azure Linux 4.0 with GCC 15) no longer transitively
# includes <cstdint> from other standard headers. Force-include it for every
# C++ translation unit so that fixed-width integer types (uint64_t, etc.) used
# in third-party headers (e.g. photon's fs/path.h at the pinned commit) resolve
# without needing to upgrade those pins. C sources (e.g. OCF) are unaffected.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include cstdint")

if ((CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64) OR (CMAKE_SYSTEM_PROCESSOR STREQUAL arm64))
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+crc -fsigned-char -fno-stack-protector -fomit-frame-pointer")
endif ()

set(CMAKE_CXX_STANDARD_LIBRARIES "-static-libgcc ${CMAKE_CXX_STANDARD_LIBRARIES}")
find_library(STATIC_LIBSTDC++ libstdc++.a PATHS /usr/lib/gcc/*/*)
if(NOT ${STATIC_LIBSTDC++} STREQUAL "STATIC_LIBSTDC++-NOTFOUND")
  set(CMAKE_CXX_STANDARD_LIBRARIES "-static-libstdc++ ${CMAKE_CXX_STANDARD_LIBRARIES}")
endif()

find_library(LIBZSTD libzstd.a PATHS /usr/lib/x86_64-linux-gnu/* /usr/lib64/*)
if(${LIBZSTD} STREQUAL "LIBZSTD-NOTFOUND")
  message("libzstd.a not found, try to find shared library")
  find_library(LIBZSTD libzstd.so PATHS /usr/lib/x86_64-linux-gnu/* /usr/lib64/*)
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED on)
set(ENABLE_MIMIC_VDSO off)

option(BUILD_CURL_FROM_SOURCE "Compile static libcurl" off)
option(BUILD_STREAM_CONVERTOR "Build the stream convertor" on)
option(ORIGIN_EXT2FS "Use original libext2fs" off)
# --- ublk kernel-support detection -----------------------------------------
# The ublk userspace block driver (ublk_drv) landed in mainline Linux 6.0.
# Default BUILD_UBLK_FRONTEND to ON only when the running kernel can drive
# ublk, so a bare `cmake ..` does not attempt the ublk build (and its
# autotools + liburing + ublksrv fetch) on kernels without ublk. It can still
# be forced either way with -DBUILD_UBLK_FRONTEND=on|off.
set(UBLK_KERNEL_SUPPORTED OFF)
set(UBLK_SUPPORT_REASON "not detected")

# (a) preferred: modinfo resolves ublk_drv for the running kernel (loaded or not)
find_program(MODINFO_EXECUTABLE modinfo PATHS /sbin /usr/sbin /bin /usr/bin)
if(MODINFO_EXECUTABLE)
  execute_process(
    COMMAND ${MODINFO_EXECUTABLE} -F filename ublk_drv
    OUTPUT_VARIABLE _ublk_drv_path ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _ublk_modinfo_rc)
  if(_ublk_modinfo_rc EQUAL 0 AND _ublk_drv_path)
    set(UBLK_KERNEL_SUPPORTED ON)
    set(UBLK_SUPPORT_REASON "ublk_drv module ${_ublk_drv_path}")
  endif()
endif()

# (b) fallback: the module file ships under /lib/modules/$(uname -r)
if(NOT UBLK_KERNEL_SUPPORTED)
  execute_process(COMMAND uname -r OUTPUT_VARIABLE _ublk_krel
    ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_ublk_krel)
    file(GLOB _ublk_drv_ko "/lib/modules/${_ublk_krel}/kernel/drivers/block/ublk_drv.ko*")
    if(_ublk_drv_ko)
      set(UBLK_KERNEL_SUPPORTED ON)
      set(UBLK_SUPPORT_REASON "ublk_drv module ${_ublk_drv_ko}")
    endif()
  endif()
endif()

# (c) fallback: the ublk uapi header is installed (kernel-headers >= 6.0)
if(NOT UBLK_KERNEL_SUPPORTED)
  find_path(UBLK_CMD_HEADER NAMES linux/ublk_cmd.h)
  if(UBLK_CMD_HEADER)
    set(UBLK_KERNEL_SUPPORTED ON)
    set(UBLK_SUPPORT_REASON "uapi header ${UBLK_CMD_HEADER}/linux/ublk_cmd.h")
  endif()
endif()

# Building the frontend additionally requires autotools on the build machine.
option(BUILD_UBLK_FRONTEND "Build the ublk frontend (overlaybd-ublk); auto-detected from kernel ublk support, requires autotools" ${UBLK_KERNEL_SUPPORTED})

if(BUILD_UBLK_FRONTEND)
  message(STATUS "ublk frontend: ON  (kernel ublk support: ${UBLK_SUPPORT_REASON})")
else()
  message(STATUS "ublk frontend: OFF (kernel ublk support: ${UBLK_SUPPORT_REASON}; force with -DBUILD_UBLK_FRONTEND=on)")
endif()

find_package(photon REQUIRED)
find_package(tcmu REQUIRED)

if(BUILD_UBLK_FRONTEND)
    find_package(liburing REQUIRED)
    find_package(ublksrv REQUIRED)
endif()

if(BUILD_STREAM_CONVERTOR)
    find_package(yaml-cpp)
    if (NOT yaml-cpp_FOUND)
        FetchContent_Declare(
            yaml-cpp
            GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
            # 0.9.0 includes the missing <cstdint> include (PR #1310) required
            # to build with GCC 15 (C23 default), e.g. on Azure Linux 4.0.
            GIT_TAG yaml-cpp-0.9.0
        )
        FetchContent_MakeAvailable(yaml-cpp)
    endif()
endif()

if(BUILD_TESTING)
  enable_testing()
  include(CTest)
endif()

add_subdirectory(src)

add_subdirectory(baselayers)

include(CMake/pack.cmake)
