find_package(gtest REQUIRED)  # GTEST::gtest
find_package(gflags REQUIRED) # GFLAGS::gflags

set(SRC_DIR ${PROJECT_SOURCE_DIR}/src)

# Every test brings its own main(), gtest_main is deliberately not linked.
function(overlaybd_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE GTEST::gtest)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

overlaybd_add_test(lsmt_test ${SRC_DIR}/overlaybd/lsmt/test/test.cpp)
target_link_libraries(lsmt_test PRIVATE overlaybd_lib GFLAGS::gflags)

overlaybd_add_test(zfile_test ${SRC_DIR}/overlaybd/zfile/test/test.cpp)
target_link_libraries(zfile_test PRIVATE overlaybd_lib GFLAGS::gflags)

overlaybd_add_test(gzindex_test ${SRC_DIR}/overlaybd/gzindex/test/test.cpp)
target_link_libraries(gzindex_test PRIVATE gzindex_lib gzip_lib gzip_cache_lib checksum_lib)

# test.cpp goes through photon's extfs
overlaybd_add_test(untar_test ${SRC_DIR}/overlaybd/tar/test/test.cpp)
target_link_libraries(untar_test PRIVATE
    tar_lib lsmt_lib gzip_lib gzindex_lib checksum_lib E2FSPROGS::libext2fs)

overlaybd_add_test(erofs_simple_test ${SRC_DIR}/overlaybd/tar/erofs/test/erofs_simple.cpp)
target_link_libraries(erofs_simple_test PRIVATE overlaybd_image_lib)

overlaybd_add_test(erofs_stress_test
    ${SRC_DIR}/overlaybd/tar/erofs/test/erofs_stress.cpp
    ${SRC_DIR}/overlaybd/tar/erofs/test/erofs_stress_base.cpp)
target_link_libraries(erofs_stress_test PRIVATE overlaybd_image_lib)

overlaybd_add_test(image_service_test ${SRC_DIR}/test/image_service_test.cpp)
target_link_libraries(image_service_test PRIVATE overlaybd_image_lib)

overlaybd_add_test(simple_credsrv_test ${SRC_DIR}/test/simple_credsrv_test.cpp)
target_link_libraries(simple_credsrv_test PRIVATE overlaybd_image_lib)

overlaybd_add_test(trace_test ${SRC_DIR}/test/trace_test.cpp)
target_link_libraries(trace_test PRIVATE overlaybd_image_lib)

# ublk_dispatch_test compiles the sources under test instead of linking
# ublk_frontend_lib: io_dispatch/cli are pure logic decoupled from the event
# loop, so the test needs neither a ublk-capable kernel nor photon. It is also
# the only test without its own main(), hence gtest_main.
if(BUILD_UBLK_FRONTEND)
    overlaybd_add_test(ublk_dispatch_test
        ${SRC_DIR}/ublk/test/ublk_dispatch_test.cpp
        ${SRC_DIR}/ublk/cli.cpp
        ${SRC_DIR}/ublk/config_patch.cpp
        ${SRC_DIR}/ublk/io_dispatch.cpp
        ${SRC_DIR}/ublk/ublkd_protocol.cpp)
    target_include_directories(ublk_dispatch_test PRIVATE
        ${UBLKSRV_INCLUDE_DIRS} ${LIBURING_INCLUDE_DIRS} ${RAPIDJSON_INCLUDE_DIRS})
    target_link_libraries(ublk_dispatch_test PRIVATE GTEST::gtest_main pthread)
endif()

# resize_test drives resize2fs directly, so it needs the objects that
# overlaybd_image_lib only re-exports for photon's resize_extfs()
if(NOT ORIGIN_EXT2FS)
    # These .o files are produced by the libext2fs_build step, so they are
    # absent at configure time. The GENERATED/EXTERNAL_OBJECT properties are
    # directory-scoped, hence set again here rather than inherited from the
    # top-level CMakeLists.txt.
    set_source_files_properties(
        ${E2FS_RESIZE_DIR}/resize2fs.o
        ${E2FS_RESIZE_DIR}/extent.o
        ${E2FS_RESIZE_DIR}/resource_track.o
        PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    overlaybd_add_test(resize_test
        ${SRC_DIR}/test/resize_test.cpp
        ${E2FS_RESIZE_DIR}/resize2fs.o
        ${E2FS_RESIZE_DIR}/extent.o
        ${E2FS_RESIZE_DIR}/resource_track.o)
    target_include_directories(resize_test PRIVATE ${E2FS_INCLUDE_DIRS} ${E2FS_RESIZE_DIR})
    target_link_libraries(resize_test PRIVATE external_lib E2FSPROGS::libext2fs)
    add_dependencies(resize_test libext2fs_build)
endif()
