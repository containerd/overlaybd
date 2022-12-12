include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(extfs_test test.cpp)
target_include_directories(extfs_test PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(extfs_test gtest gtest_main gflags pthread photon_static overlaybd_lib)

add_test(
    NAME extfs_test
    COMMAND ${EXECUTABLE_OUTPUT_PATH}/extfs_test
)
