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


add_executable(simple_credsrv_test simple_credsrv_test.cpp)
add_test(
    NAME simple_credsrv_test
    COMMAND ${EXECUTABLE_OUTPUT_PATH}/simple_credsrv_test
)
target_link_libraries(simple_credsrv_test
overlaybd_image_lib
photon_static
rt
resolv
aio
pthread
gtest
)

target_include_directories(simple_credsrv_test PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${rapidjson_SOURCE_DIR}/include
    $ENV{GTEST}/googletest/include
)