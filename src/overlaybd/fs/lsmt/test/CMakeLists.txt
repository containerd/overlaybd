include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)



add_executable(lsmt_test test.cpp)
target_link_libraries(lsmt_test gtest gtest_main gflags pthread fs_lib base_lib photon_lib
  -laio -lrt )

add_test(
  NAME lsmt_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/lsmt_test
)

