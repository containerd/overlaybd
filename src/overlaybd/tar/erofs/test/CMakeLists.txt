include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(erofs_test test.cpp)
target_include_directories(erofs_test PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(erofs_test gtest gtest_main pthread photon_static
  tar_lib lsmt_lib gzip_lib gzindex_lib checksum_lib overlaybd_image_lib)

target_include_directories(erofs_test PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)

add_test(
  NAME erofs_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/erofs_test
)
