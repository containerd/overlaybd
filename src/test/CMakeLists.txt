include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

add_executable(image_service_test image_service_test.cpp)
target_include_directories(image_service_test PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)
target_link_libraries(image_service_test GTest::gtest GTest::gtest_main gflags pthread photon_static overlaybd_lib overlaybd_image_lib)

add_test(
    NAME image_service_test
    COMMAND ${EXECUTABLE_OUTPUT_PATH}/image_service_test
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
GTest::gtest
)

target_include_directories(simple_credsrv_test PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)

add_executable(trace_test trace_test.cpp ../tools/comm_func.cpp)
target_include_directories(trace_test PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)
target_link_libraries(trace_test GTest::gtest GTest::gtest_main gflags pthread photon_static overlaybd_lib overlaybd_image_lib)

add_test(
    NAME trace_test
    COMMAND ${EXECUTABLE_OUTPUT_PATH}/trace_test
)
