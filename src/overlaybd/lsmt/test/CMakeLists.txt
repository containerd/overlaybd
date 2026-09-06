include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(lsmt_test test.cpp inner_search_test.cpp)
target_include_directories(lsmt_test PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(lsmt_test gtest gtest_main gflags pthread photon_static overlaybd_lib)

# Recent googletest releases (e.g. Fedora 44) require C++17; the library
# itself stays on the project-wide C++14.
set_property(TARGET lsmt_test PROPERTY CXX_STANDARD 17)

add_test(
  NAME lsmt_test
  COMMAND ${EXECUTABLE_OUTPUT_PATH}/lsmt_test
)

