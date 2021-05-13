include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(fs_test test.cpp)
target_link_libraries(fs_test gtest gtest_main gmock gflags pthread fs_lib base_lib photon_lib
    -laio -lrt)

add_test(
  NAME fs_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/fs_test
)

add_executable(throttledfile_test test_throttledfile.cpp)
target_link_libraries(throttledfile_test gtest gtest_main gmock gflags pthread fs_lib base_lib photon_lib
    -laio -lrt)

add_test(
  NAME throttledfile_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/throttledfile_test
)

