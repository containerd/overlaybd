include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)


add_executable(lsmt_test test.cpp)
target_include_directories(lsmt_test PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(lsmt_test GTest::gtest GTest::gtest_main gflags pthread photon_static overlaybd_lib)

add_test(
  NAME lsmt_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/lsmt_test
)

