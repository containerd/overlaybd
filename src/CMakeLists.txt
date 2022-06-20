find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(aio REQUIRED)
find_package(rapidjson REQUIRED)

link_libraries(rt pthread resolv)

add_subdirectory(overlaybd)

add_executable(overlaybd-tcmu
  main.cpp
  image_file.cpp
  image_service.cpp
  sure_file.cpp
  switch_file.cpp
  bk_download.cpp
  prefetch.cpp
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

add_subdirectory(tools)