include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

# Compile only the sources under test instead of linking ublk_frontend_lib:
# io_dispatch/cli are pure logic decoupled from the event loop, so the tests
# need neither a ublk-capable kernel nor photon
add_executable(ublk_dispatch_test
    ublk_dispatch_test.cpp
    ../io_dispatch.cpp
    ../cli.cpp
    ../config_patch.cpp
    ../ublkd_protocol.cpp
)
target_include_directories(ublk_dispatch_test PUBLIC
    ${UBLKSRV_INCLUDE_DIRS} ${LIBURING_INCLUDE_DIRS} ${RAPIDJSON_INCLUDE_DIRS})
target_link_libraries(ublk_dispatch_test gtest gtest_main pthread)

add_test(
    NAME ublk_dispatch_test
    COMMAND ${EXECUTABLE_OUTPUT_PATH}/ublk_dispatch_test
)

# Exercises the real Photon file cache; no ublk device or privileged kernel
# operations are needed. Keep assertions enabled in Release test builds.
add_executable(ublkd_cache_import_test cache_import_test.cpp)
target_compile_options(ublkd_cache_import_test PRIVATE -UNDEBUG)
target_link_libraries(ublkd_cache_import_test
    ublk_frontend_lib
    photon_static
    overlaybd_image_lib
    libublksrv_static
    ${CURL_LIBRARIES}
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${AIO_LIBRARIES}
)
add_test(NAME ublkd_cache_import_test
    COMMAND ${EXECUTABLE_OUTPUT_PATH}/ublkd_cache_import_test)
set_tests_properties(ublkd_cache_import_test PROPERTIES TIMEOUT 180)
