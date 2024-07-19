find_package(curl REQUIRED)
find_package(openssl REQUIRED)
find_package(aio REQUIRED)
find_package(rapidjson REQUIRED)

link_libraries(rt pthread resolv)

add_subdirectory(overlaybd)

add_library(overlaybd_image_lib
  image_file.cpp
  image_service.cpp
  switch_file.cpp
  bk_download.cpp
  prefetch.cpp
  tools/sha256file.cpp
  tools/comm_func.cpp
)
target_include_directories(overlaybd_image_lib PUBLIC
  ${CURL_INCLUDE_DIRS}
  ${OPENSSL_INCLUDE_DIR}
  ${rapidjson_SOURCE_DIR}/include
  ${PHOTON_INCLUDE_DIR}
)

target_link_libraries(overlaybd_image_lib
  photon_static
  overlaybd_lib
  ${CURL_LIBRARIES}
  ${OPENSSL_SSL_LIBRARY}
  ${OPENSSL_CRYPTO_LIBRARY}
  ${AIO_LIBRARIES}
)

add_executable(overlaybd-tcmu
  main.cpp
)
target_include_directories(overlaybd-tcmu PUBLIC
  ${TCMU_INCLUDE_DIR}
  ${CURL_INCLUDE_DIRS}
  ${OPENSSL_INCLUDE_DIR}
  ${rapidjson_SOURCE_DIR}/include
  ${PHOTON_INCLUDE_DIR}
)
target_link_libraries(overlaybd-tcmu
  photon_static
  overlaybd_lib
  overlaybd_image_lib
  tcmu_static
  ${CURL_LIBRARIES}
  ${OPENSSL_SSL_LIBRARY}
  ${OPENSSL_CRYPTO_LIBRARY}
  ${AIO_LIBRARIES}
)

install(TARGETS overlaybd-tcmu DESTINATION /opt/overlaybd/bin)
install(FILES example_config/overlaybd-tcmu.service DESTINATION /opt/overlaybd/)
install(FILES example_config/overlaybd.json DESTINATION /etc/overlaybd/)
install(FILES example_config/cred.json DESTINATION /opt/overlaybd/)
if (NOT ORIGIN_EXT2FS)
    install(DIRECTORY ${LIBEXT2FS_INSTALL_DIR}/lib DESTINATION /opt/overlaybd/ USE_SOURCE_PERMISSIONS)
endif()

add_subdirectory(tools)
if (BUILD_TESTING)
    add_subdirectory(test)
endif ()
