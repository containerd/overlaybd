include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

# erofs simple test
add_executable(erofs_simple_test erofs_simple.cpp)
target_include_directories(erofs_simple_test PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(erofs_simple_test gtest gtest_main pthread photon_static
  tar_lib lsmt_lib gzip_lib gzindex_lib checksum_lib overlaybd_image_lib)

target_include_directories(erofs_simple_test PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)

add_test(
  NAME erofs_simple_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/erofs_simple_test
)

# erofs stress test
add_executable(erofs_stress_test erofs_stress.cpp erofs_stress_base.cpp)
target_include_directories(erofs_stress_test PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(erofs_stress_test gtest gtest_main pthread photon_static
  tar_lib lsmt_lib gzip_lib gzindex_lib checksum_lib overlaybd_image_lib)

target_include_directories(erofs_stress_test PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)

add_test(
  NAME erofs_stress_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/erofs_stress_test
)
