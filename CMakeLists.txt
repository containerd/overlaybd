cmake_minimum_required(VERSION 3.15)

project(
  overlaybd
  VERSION 0.5.1
  LANGUAGES C CXX
)
enable_language(C)

set(LIBRARY_OUTPUT_PATH "${CMAKE_SOURCE_DIR}/build/output")
set(EXECUTABLE_OUTPUT_PATH "${CMAKE_SOURCE_DIR}/build/output")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fpic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpic")

set(CMAKE_CXX_STANDARD_LIBRARIES "-static-libgcc ${CMAKE_CXX_STANDARD_LIBRARIES}")
find_library(STATIC_LIBSTDC++ libstdc++.a PATHS /usr/lib/gcc/*/*)
if(NOT ${STATIC_LIBSTDC++} STREQUAL "STATIC_LIBSTDC++-NOTFOUND")
  set(CMAKE_CXX_STANDARD_LIBRARIES "-static-libstdc++ ${CMAKE_CXX_STANDARD_LIBRARIES}")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED on)

find_package(photon REQUIRED)
find_package(tcmu REQUIRED)

if(BUILD_TESTING)
  enable_testing()
  include(CTest)
endif()

add_subdirectory(src)
add_subdirectory(baselayers)

include(CMake/pack.cmake)
