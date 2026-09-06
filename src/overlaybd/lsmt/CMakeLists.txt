file(GLOB SOURCE_LSMT "*.cpp")

# index_sve.cpp must not be compiled unconditionally: it requires a
# toolchain that supports SVE code generation (gcc >= 10, via ACLE
# <arm_sve.h>). Exclude it from the glob and re-add only when the
# toolchain probe passes, so older toolchains (e.g. devtoolset-7)
# keep building without it.
list(REMOVE_ITEM SOURCE_LSMT "${CMAKE_CURRENT_SOURCE_DIR}/index_sve.cpp")

add_library(lsmt_lib STATIC ${SOURCE_LSMT})
target_include_directories(lsmt_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  option(OVERLAYBD_DISABLE_SVE "Force-disable LSMT SVE acceleration" OFF)
  include(CheckCXXCompilerFlag)
  include(CheckCXXSourceCompiles)
  if(NOT OVERLAYBD_DISABLE_SVE)
  check_cxx_compiler_flag("-march=armv8.2-a+sve" COMPILER_HAS_SVE_FLAG)
  set(SVE_PROBE_SOURCE "#include <arm_sve.h>
#include <cstdint>
extern \"C\" uint32_t probe(const uint32_t *p, uint32_t x) {
  svbool_t pg = svwhilelt_b32(0UL, 4UL);
  svuint32_t d = svld1_u32(pg, p);
  svbool_t c = svcmple_u32(pg, d, svdup_u32(x));
  return (uint32_t)svaddv_u32(c, svdup_u32(1));
}
int main() { return 0; }
")
  set(CMAKE_REQUIRED_FLAGS "-march=armv8.2-a+sve")
  check_cxx_source_compiles("${SVE_PROBE_SOURCE}" SVE_HEADER_USABLE)
  unset(CMAKE_REQUIRED_FLAGS)
  if(COMPILER_HAS_SVE_FLAG AND SVE_HEADER_USABLE)
    target_sources(lsmt_lib PRIVATE index_sve.cpp)
    # armv8.2-a is the minimal architecture baseline that can carry the
    # +sve extension -- a permission floor for code generation, not a
    # per-generation target: newer SVE hardware runs this same object
    # unchanged, and runtime dispatch (HWCAP_SVE) gates execution.
    set_source_files_properties(index_sve.cpp PROPERTIES COMPILE_OPTIONS "-march=armv8.2-a+sve")
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
