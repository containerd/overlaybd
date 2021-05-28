cmake_minimum_required(VERSION 3.11)

project(
  overlaybd
  VERSION 0.3.0
  LANGUAGES C CXX
)
enable_language(C ASM)

set(LIBRARY_OUTPUT_PATH "${CMAKE_SOURCE_DIR}/build/output")
set(EXECUTABLE_OUTPUT_PATH "${CMAKE_SOURCE_DIR}/build/output")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fpic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpic")

add_definitions("-DRAPIDJSON_HAS_STDSTRING=1")

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED on)

if(BUILD_TESTING)
  enable_testing()
  include(CTest)
endif()

add_subdirectory(src)
add_subdirectory(baselayers)

include(CMake/pack.cmake)
