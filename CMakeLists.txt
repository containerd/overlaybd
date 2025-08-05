cmake_minimum_required(VERSION 3.14)

project(
  overlaybd
  LANGUAGES C CXX
)

include(FetchContent)
enable_language(C)

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -DNDEBUG -g")

# Get CPU arch
execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT (${ARCH} STREQUAL x86_64) AND NOT (${ARCH} STREQUAL aarch64) AND NOT (${ARCH} STREQUAL arm64))
    message(FATAL_ERROR "Unknown CPU architecture ${ARCH}")
endif ()

option(OBD_VER "Overlaybd version" "overlaybd/0.0.0-undefined")

set(LIBRARY_OUTPUT_PATH "${CMAKE_SOURCE_DIR}/build/output")
set(EXECUTABLE_OUTPUT_PATH "${CMAKE_SOURCE_DIR}/build/output")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fpic -Wall -Werror=sign-compare")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpic -Wall -Werror=sign-compare -DOVERLAYBD_VER=${OBD_VER}")

if (${ARCH} STREQUAL aarch64)
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
option(ORIGIN_EXT2FS "Use original libext2fs" off)
find_package(photon REQUIRED)
find_package(tcmu REQUIRED)
find_package(yamlcpp)

# OpenTelemetry
find_package(opentelemetry-cpp REQUIRED)
if (NOT opentelemetry-cpp_FOUND)
  FetchContent_Declare(
    opentelemetry-cpp
    GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
    GIT_TAG v1.14.2
  )
  set(WITH_OTLP_HTTP ON)
  set(WITH_OTLP_GRPC ON)
  set(WITH_JAEGER ON)
  set(WITH_ZIPKIN ON)
  set(BUILD_TESTING OFF)
  set(OTELCPP_PROTO_PATH ${CMAKE_CURRENT_BINARY_DIR}/_deps/opentelemetry-cpp-src/third_party/opentelemetry-proto)
  FetchContent_MakeAvailable(opentelemetry-cpp)
endif()
if (NOT yamlcpp_FOUND)
  FetchContent_Declare(
    yamlcpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0
  )
  FetchContent_MakeAvailable(yamlcpp)
endif()


if(BUILD_TESTING)
  enable_testing()
  include(CTest)
endif()

add_subdirectory(src)

add_subdirectory(baselayers)

include(CMake/pack.cmake)
