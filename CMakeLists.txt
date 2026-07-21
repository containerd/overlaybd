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

find_package(photon REQUIRED)
find_package(tcmu REQUIRED)

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
