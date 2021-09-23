include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)


add_executable(crc-test crc-test.cpp)
target_link_libraries(crc-test pthread fs_lib base_lib photon_lib
        -laio -lrt)