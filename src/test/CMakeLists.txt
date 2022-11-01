cmake_minimum_required(VERSION 3.11)

find_package(rapidjson REQUIRED)

set(CURL_STATICLIB on)
add_definitions(-DCURL_STATICLIB)

INCLUDE_DIRECTORIES(
    ${PHOTON_INCLUDE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)

add_executable(simple_auth_srv simple_auth_server.cpp)

target_link_libraries(simple_auth_srv
photon_static
rt
resolv
aio
pthread
)
