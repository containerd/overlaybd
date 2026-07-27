# overlaybd-ublk: the ublk block device frontend (one process serves one device)
# Entered only when BUILD_UBLK_FRONTEND=on

add_library(ublk_frontend_lib
    io_dispatch.cpp
    ublk_device.cpp
    cli.cpp
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

if (BUILD_TESTING)
    add_subdirectory(test)
endif ()
