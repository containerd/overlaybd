include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(cache_test cache_test.cpp)
target_include_directories(cache_test PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(cache_test gtest gtest_main gflags pthread photon_static overlaybd_lib)

add_test(
  NAME cache_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/cache_test
)