file(GLOB SOURCE_LSMT "*.cpp")

add_library(lsmt_lib STATIC ${SOURCE_LSMT})
target_include_directories(lsmt_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  option(OVERLAYBD_DISABLE_SVE "Force-disable LSMT SVE acceleration" OFF)
  include(CheckCXXSourceCompiles)
  if(NOT OVERLAYBD_DISABLE_SVE)
  # The SVE kernels live in index.cpp under OVERLAYBD_ENABLE_SVE,
  # scoped by a #pragma GCC target region. Probe the same
  # pragma+intrinsics shape with the build's own compiler, without
  # overriding -march, to ensure SVE is supported at code-block scope.
  set(SVE_PROBE_SOURCE "#include <arm_sve.h>
#include <cstdint>
#pragma GCC push_options
#pragma GCC target(\"arch=armv8.2-a+sve\")
extern \"C\" uint32_t probe(const uint32_t *p, uint32_t x) {
  svbool_t pg = svwhilelt_b32(0UL, 4UL);
  svuint32_t d = svld1_u32(pg, p);
  svbool_t c = svcmple_u32(pg, d, svdup_u32(x));
  return (uint32_t)svaddv_u32(c, svdup_u32(1));
}
#pragma GCC pop_options
int main() { return 0; }
")
  check_cxx_source_compiles("${SVE_PROBE_SOURCE}" SVE_TARGET_PRAGMA_USABLE)
  if(SVE_TARGET_PRAGMA_USABLE)
    target_compile_definitions(lsmt_lib PUBLIC OVERLAYBD_ENABLE_SVE)
    message(STATUS "LSMT SVE acceleration: ENABLED")
  else()
    message(STATUS "LSMT SVE acceleration: DISABLED (toolchain probe failed)")
  endif()
  endif() # OVERLAYBD_DISABLE_SVE
endif()

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
