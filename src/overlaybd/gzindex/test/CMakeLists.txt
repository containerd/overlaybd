include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)


add_executable(gzindex_test test.cpp)
target_include_directories(gzindex_test PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(gzindex_test GTest::gtest GTest::gtest_main gflags pthread photon_static
     gzindex_lib gzip_lib cache_lib checksum_lib)

add_test(
    NAME gzindex_test
    COMMAND ${EXECUTABLE_OUTPUT_PATH}/gzindex_test
)
