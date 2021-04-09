add_subdirectory(overlaybd)
add_subdirectory(tools)




find_package(PkgConfig)
pkg_check_modules(GLIB REQUIRED gio-unix-2.0)

find_library(LIBNL_LIB nl-3)
find_library(LIBNL_GENL_LIB nl-genl-3)
set(LIBNL_LIBS
  ${LIBNL_LIB}
  ${LIBNL_GENL_LIB}
)

find_path (LIBNL_INCLUDE_DIR
  NAMES
  netlink/netlink.h
  PATH_SUFFIXES
  libnl3
)

add_library(tcmu_lib
    STATIC
    tcmu-runner/strlcpy.c
    tcmu-runner/configfs.c
    tcmu-runner/api.c
    tcmu-runner/libtcmu.c
    tcmu-runner/libtcmu_log.c
    tcmu-runner/libtcmu_config.c
    tcmu-runner/libtcmu_time.c
    tcmu-runner/scsi.c
)
target_include_directories(tcmu_lib
  PUBLIC ${LIBNL_INCLUDE_DIR}
  PUBLIC ${GLIB_INCLUDE_DIRS}
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/tcmu-runner/ccan
)
target_link_libraries(tcmu_lib
  ${LIBNL_LIBS}
  ${GLIB_LIBRARIES}
)




set(CURL_STATIC ON)
find_package(CURL REQUIRED)
set(OpenSSL_STATIC ON)
find_package(OpenSSL REQUIRED)

file(GLOB SOURCE_IMAGE "image_file.cpp" "image_service.cpp" "sure_file.cpp" "switch_file.cpp" "bk_download.cpp")

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

add_executable(overlaybd-tcmu main.cpp scsi_helper.cpp)

add_dependencies(overlaybd-tcmu RapidJSON)

target_link_libraries(overlaybd-tcmu
    -Wl,--whole-archive
    base_lib
    image_lib
    fs_lib
    photon_lib
    net_lib
    tcmu_lib
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


install(TARGETS overlaybd-tcmu DESTINATION /opt/overlaybd/bin)
install(FILES example_config/overlaybd.json DESTINATION /etc/overlaybd/)
install(FILES example_config/cred.json DESTINATION /opt/overlaybd/)
