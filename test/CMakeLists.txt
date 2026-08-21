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

overlaybd_add_test(cache_test ${SRC_DIR}/overlaybd/cache/test/cache_test.cpp)
target_link_libraries(cache_test PRIVATE overlaybd_lib)

overlaybd_add_test(gzindex_test ${SRC_DIR}/overlaybd/gzindex/test/test.cpp)
target_link_libraries(gzindex_test PRIVATE gzindex_lib gzip_lib cache_lib checksum_lib)

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

# ocf_perf_test is a benchmark rather than a unit test, --ut_pass makes it
# report success without running the workload
if(ENABLE_OCF_CACHE)
    add_executable(ocf_perf_test ${SRC_DIR}/overlaybd/cache/ocf_cache/test/ocf_perf_test.cpp)
    target_link_libraries(ocf_perf_test PRIVATE overlaybd_lib GTEST::gtest GFLAGS::gflags)
    add_test(NAME ocf_perf_test COMMAND ocf_perf_test --ut_pass=true)
endif()
