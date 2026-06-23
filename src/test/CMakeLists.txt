include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(image_service_test image_service_test.cpp)
target_include_directories(image_service_test PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${RAPIDJSON_INCLUDE_DIRS}
)
target_link_libraries(image_service_test gtest gtest_main gflags pthread photon_static overlaybd_lib overlaybd_image_lib)

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
gtest
)

target_include_directories(simple_credsrv_test PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${RAPIDJSON_INCLUDE_DIRS}
    $ENV{GTEST}/googletest/include
)

add_executable(trace_test trace_test.cpp ../tools/comm_func.cpp)
target_include_directories(trace_test PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${RAPIDJSON_INCLUDE_DIRS}
)
target_link_libraries(trace_test gtest gtest_main gflags pthread photon_static overlaybd_lib overlaybd_image_lib)

add_test(
    NAME trace_test
    COMMAND ${EXECUTABLE_OUTPUT_PATH}/trace_test
)

if (NOT ORIGIN_EXT2FS)
    set_source_files_properties(
        ${E2FS_RESIZE_DIR}/resize2fs.o
        ${E2FS_RESIZE_DIR}/extent.o
        ${E2FS_RESIZE_DIR}/resource_track.o
        PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    add_executable(resize_test
        resize_test.cpp
        ${E2FS_RESIZE_DIR}/resize2fs.o
        ${E2FS_RESIZE_DIR}/extent.o
        ${E2FS_RESIZE_DIR}/resource_track.o
    )
    target_include_directories(resize_test PUBLIC
        ${PHOTON_INCLUDE_DIR}
        ${E2FS_INCLUDE_DIRS}
        ${E2FS_RESIZE_DIR}
    )
    target_link_libraries(resize_test gtest pthread photon_static ${E2FS_LIBRARIES})
    add_dependencies(resize_test libext2fs_build)
    add_test(
        NAME resize_test
        COMMAND ${EXECUTABLE_OUTPUT_PATH}/resize_test
    )
endif()
