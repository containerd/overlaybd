find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(aio REQUIRED)
find_package(nl)
find_package(PkgConfig)
pkg_check_modules(GLIB REQUIRED gio-unix-2.0)

link_libraries(pthread aio rt resolv)

include(FetchContent)
set(FETCHCONTENT_QUIET false)

FetchContent_Declare(
  rapidjson
  URL https://github.com/Tencent/rapidjson/archive/v1.1.0.tar.gz
  URL_MD5 badd12c511e081fec6c89c43a7027bce
)
FetchContent_GetProperties(rapidjson)
if (NOT rapidjson_POPULATED)
  FetchContent_Populate(rapidjson)
endif()

FetchContent_Declare(
  tcmu_runner
  GIT_REPOSITORY https://github.com/open-iscsi/tcmu-runner.git
  GIT_TAG 364ae611ffdd7c8fb3d1e733563b636bf3b44ba6
  PATCH_COMMAND patch -p1 < ${CMAKE_CURRENT_SOURCE_DIR}/libtcmu.c.diff
)
FetchContent_GetProperties(tcmu_runner)
if (NOT tcmu_runner_POPULATED)
  FetchContent_Populate(tcmu_runner)
endif()

add_subdirectory(overlaybd)

add_library(tcmu_lib
    STATIC
    ${tcmu_runner_SOURCE_DIR}/strlcpy.c
    ${tcmu_runner_SOURCE_DIR}/configfs.c
    ${tcmu_runner_SOURCE_DIR}/api.c
    ${tcmu_runner_SOURCE_DIR}/libtcmu.c
    ${tcmu_runner_SOURCE_DIR}/libtcmu_log.c
    ${tcmu_runner_SOURCE_DIR}/libtcmu_config.c
    ${tcmu_runner_SOURCE_DIR}/libtcmu_time.c
    ${tcmu_runner_SOURCE_DIR}/scsi.c
)
target_include_directories(tcmu_lib
  PUBLIC ${LIBNL_INCLUDE_DIR}
  PUBLIC ${GLIB_INCLUDE_DIRS}
  PUBLIC ${tcmu_runner_SOURCE_DIR}/ccan
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(tcmu_lib
  ${LIBNL_LIBS}
  ${GLIB_LIBRARIES}
  photon_lib
)

file(GLOB SOURCE_IMAGE image_file.cpp image_service.cpp sure_file.cpp switch_file.cpp bk_download.cpp prefetch.cpp)
add_library(image_lib STATIC
    ${SOURCE_IMAGE}
)
target_include_directories(image_lib PUBLIC
    ${CURL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)

add_executable(overlaybd-tcmu main.cpp)
target_include_directories(overlaybd-tcmu
  PUBLIC ${tcmu_runner_SOURCE_DIR}
)
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
)

install(TARGETS overlaybd-tcmu DESTINATION /opt/overlaybd/bin)
install(FILES example_config/overlaybd-tcmu.service DESTINATION /opt/overlaybd/)
install(FILES example_config/overlaybd.json DESTINATION /etc/overlaybd/)
install(FILES example_config/cred.json DESTINATION /opt/overlaybd/)

add_subdirectory(tools)