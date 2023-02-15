include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(curl_test test.cpp)
target_include_directories(curl_test PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)
target_link_libraries(curl_test gtest gtest_main gflags pthread photon_static overlaybd_lib overlaybd_image_lib)

add_test(
    NAME curl_test
    COMMAND ${EXECUTABLE_OUTPUT_PATH}/curl_test
)