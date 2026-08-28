# libtcmu is built as part of the OverlayBD project; photon_static and the
# CMake module path (for Findnl) are provided by the top-level CMakeLists.

find_package(nl REQUIRED)

add_library(tcmu_static
  configfs.cpp
  api.cpp
  libtcmu.cpp
  scsi.cpp
)
set_target_properties(tcmu_static PROPERTIES OUTPUT_NAME tcmu)
target_include_directories(tcmu_static
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC ${LIBNL_INCLUDE_DIR}
  PUBLIC ${PHOTON_INCLUDE_DIR}
)
target_compile_options(tcmu_static PRIVATE -fpermissive)
target_link_libraries(tcmu_static
  ${LIBNL_LIB}
  ${LIBNL_GENL_LIB}
  photon_static
)
