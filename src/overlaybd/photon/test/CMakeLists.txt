
include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(photon_test test.cpp)
target_link_libraries(photon_test gtest gtest_main gflags pthread base_lib photon_lib
    -laio -lrt)

add_test(
  NAME photon_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/photon_test
)

add_executable(syncio_test test-syncio.cpp)
target_link_libraries(syncio_test gtest gtest_main gflags pthread base_lib photon_lib
    -laio -lrt)

add_test(
  NAME syncio_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/photon_test
)

add_executable(signalfd_test signalfdtest.cpp)
target_link_libraries(signalfd_test gtest gtest_main gflags pthread base_lib photon_lib
    -laio -lrt)

add_test(
  NAME signalfd_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/photon_test
)
