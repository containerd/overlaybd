add_executable(overlaybd-commit overlaybd-commit.cpp)

target_link_libraries(overlaybd-commit
    -Wl,--whole-archive
    base_lib
    fs_lib
    photon_lib
    -Wl,--no-whole-archive
    -laio
    -lrt
    -lresolv
    -lpthread
    -static-libgcc
)

add_executable(overlaybd-create overlaybd-create.cpp)

target_link_libraries(overlaybd-create
    -Wl,--whole-archive
    base_lib
    fs_lib
    photon_lib
    -Wl,--no-whole-archive
    -laio
    -lrt
    -lresolv
    -lpthread
    -static-libgcc
)

set(CURL_STATIC ON)
find_package(CURL REQUIRED)

set(CURL_STATIC ON)
find_package(OpenSSL REQUIRED)


add_executable(overlaybd-info overlaybd-info.cpp)
add_dependencies(overlaybd-info RapidJSON)
target_include_directories(overlaybd-info PUBLIC
    ${CURL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${RAPIDJSON_INCLUDE_DIR}
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
    -laio
    -lrt
    -lresolv
    -lpthread
    -ldl
    -static-libgcc
)

add_executable(overlaybd-zfile overlaybd-zfile.cpp)

target_link_libraries(overlaybd-zfile
    -Wl,--whole-archive
    base_lib
    fs_lib
    photon_lib
    -Wl,--no-whole-archive
    -laio
    -lrt
    -lresolv
    -lpthread
    -static-libgcc
)

add_executable(overlaybd-check overlaybd-check.cpp)
add_dependencies(overlaybd-check RapidJSON)
target_include_directories(overlaybd-check PUBLIC
    ${CURL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${RAPIDJSON_INCLUDE_DIR}
)
target_link_libraries(overlaybd-check
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
    -laio
    -lrt
    -lresolv
    -lpthread
    -ldl
    -static-libgcc
)

install(TARGETS
    overlaybd-commit
    overlaybd-create
    overlaybd-info
    overlaybd-zfile
    overlaybd-check
    DESTINATION /opt/overlaybd/bin
)
