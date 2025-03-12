include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(elink_test test.cpp ../../../tools/sha256file.cpp)
target_include_directories(elink_test PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(elink_test 
    photon_static
    yaml-cpp
    gtest gtest_main gflags 
    pthread
    elink_lib
)

add_test(
  NAME elink_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/elink_test
)

