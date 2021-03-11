include(ExternalProject)

ExternalProject_Add(
  Boost
  PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/external/boost
  URL https://dl.bintray.com/boostorg/release/1.75.0/source/boost_1_75_0.tar.gz
  UPDATE_COMMAND ""
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
)

ExternalProject_Get_Property(Boost source_dir)
set(BOOST_INCLUDE_DIR ${source_dir})