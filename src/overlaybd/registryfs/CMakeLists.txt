file(GLOB SOURCE_REGISTRYFS "*.cpp")

find_package(curl REQUIRED)

add_library(registryfs_lib STATIC ${SOURCE_REGISTRYFS})
target_include_directories(registryfs_lib PUBLIC
    ${CURL_INCLUDE_DIRS}
    ${rapidjson_SOURCE_DIR}/include
    ${PHOTON_INCLUDE_DIR}
)
