
include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(zfile_test ./test.cpp)

target_link_libraries(zfile_test gtest gtest_main gflags pthread photon_static overlaybd_lib)
target_include_directories(zfile_test PUBLIC ${PHOTON_INCLUDE_DIR})
add_test(
  NAME zfile_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/zfile_test
)