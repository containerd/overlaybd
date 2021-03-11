add_subdirectory(overlaybd)
add_subdirectory(tools)

file(GLOB SOURCE_BS "*.c")
include_directories("tgt-headers")
add_library(bs-overlaybd SHARED ${SOURCE_BS})

set(CURL_STATIC ON)
find_package(CURL REQUIRED)
set(OpenSSL_STATIC ON)
find_package(OpenSSL REQUIRED)

file(GLOB SOURCE_IMAGE "*.cpp")

add_library(image_lib STATIC 
    ${SOURCE_IMAGE}
)
add_dependencies(image_lib Boost RapidJSON)

target_include_directories(image_lib PUBLIC
    ${CURL_INCLUDE_DIRS} 
    ${OPENSSL_INCLUDE_DIR}
    ${BOOST_INCLUDE_DIR}
    ${RAPIDJSON_INCLUDE_DIR}
)

target_link_libraries(bs-overlaybd
    -Wl,--whole-archive 
    base_lib
    image_lib
    fs_lib
    photon_lib
    net_lib
    ${CURL_LIBRARIES}
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    -Wl,--no-whole-archive
    
    -lrt
    -lresolv
    -laio
    -lpthread
    -static-libgcc
)

install(TARGETS bs-overlaybd DESTINATION /usr/lib64/tgt/backing-store/)
install(TARGETS bs-overlaybd DESTINATION /usr/lib/tgt/backing-store/)

install(FILES example_config/tgt-overlaybd.json DESTINATION /etc/overlaybd/)
install(FILES example_config/cred.json DESTINATION /opt/overlaybd/)


