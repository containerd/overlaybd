# overlaybd-ublk: the ublk block device frontend (one process serves one device)
# Entered only when BUILD_UBLK_FRONTEND=on

add_library(ublk_frontend_lib
    io_dispatch.cpp
    ublk_device.cpp
    cli.cpp
    config_patch.cpp
    ublkd_protocol.cpp
    pool_placeholder.cpp
    blkdev_hygiene.cpp
)
target_include_directories(ublk_frontend_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${RAPIDJSON_INCLUDE_DIRS}
)
target_link_libraries(ublk_frontend_lib
    photon_static
    overlaybd_image_lib
    libublksrv_static      # IMPORTED target; include dirs and liburing propagate via INTERFACE
)

add_executable(overlaybd-ublk main.cpp)
target_link_libraries(overlaybd-ublk
    ublk_frontend_lib
    photon_static
    overlaybd_image_lib
    libublksrv_static
    ${CURL_LIBRARIES}
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${AIO_LIBRARIES}
)

install(TARGETS overlaybd-ublk DESTINATION /opt/overlaybd/bin)

# overlaybd-ublkd: daemon mode, all devices in one process
add_executable(overlaybd-ublkd ublkd_main.cpp)
target_link_libraries(overlaybd-ublkd
    ublk_frontend_lib
    photon_static
    overlaybd_image_lib
    libublksrv_static
    ${CURL_LIBRARIES}
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${AIO_LIBRARIES}
)
install(TARGETS overlaybd-ublkd DESTINATION /opt/overlaybd/bin)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../example_config/overlaybd-ublkd.service
        DESTINATION /opt/overlaybd/)

if (BUILD_TESTING)
    add_subdirectory(test)
endif ()
