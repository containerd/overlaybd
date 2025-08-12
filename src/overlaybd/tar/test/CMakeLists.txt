include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)


add_executable(untar_test test.cpp)
target_include_directories(untar_test PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(untar_test GTest::gtest GTest::gtest_main pthread photon_static
  tar_lib lsmt_lib gzip_lib gzindex_lib checksum_lib)

add_test(
  NAME untar_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/untar_test
)
