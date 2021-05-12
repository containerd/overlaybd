
include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(base_test test.cpp)
target_link_libraries(base_test gtest gtest_main gflags pthread base_lib photon_lib
    -laio -lrt)

add_test(
  NAME base_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/base_test
)

add_executable(scalepool_test test_scalepool.cpp)
target_link_libraries(scalepool_test gtest gtest_main gflags pthread base_lib photon_lib
    -laio -lrt)

add_test(
  NAME scalepool_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/scalepool_test
)
