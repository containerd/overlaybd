find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(overlaybd-commit overlaybd-commit.cpp)
target_link_libraries(overlaybd-commit
    -Wl,--whole-archive
    base_lib
    fs_lib
    photon_lib
    -Wl,--no-whole-archive
)

add_executable(overlaybd-create overlaybd-create.cpp)
target_link_libraries(overlaybd-create
    -Wl,--whole-archive
    base_lib
    fs_lib
    photon_lib
    -Wl,--no-whole-archive
)

add_executable(overlaybd-info overlaybd-info.cpp)
target_include_directories(overlaybd-info PUBLIC
    ${CURL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)
target_link_libraries(overlaybd-info
    -Wl,--whole-archive
    base_lib
    fs_lib
    photon_lib
    net_lib
    image_lib
    ${CURL_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    -Wl,--no-whole-archive
)

add_executable(overlaybd-zfile overlaybd-zfile.cpp)
target_link_libraries(overlaybd-zfile
    -Wl,--whole-archive
    base_lib
    fs_lib
    photon_lib
    -Wl,--no-whole-archive
)

install(TARGETS
    overlaybd-commit
    overlaybd-create
    overlaybd-info
    overlaybd-zfile
    DESTINATION /opt/overlaybd/bin
)
