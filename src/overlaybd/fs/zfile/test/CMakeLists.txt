
include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(zfile_test ./test.cpp)
target_link_libraries(zfile_test gtest gtest_main gflags pthread fs_lib base_lib photon_lib
    -laio -lrt)

add_test(
  NAME zfile_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/zfile_test
)