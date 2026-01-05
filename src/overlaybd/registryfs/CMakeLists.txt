file(GLOB SOURCE_REGISTRYFS "*.cpp")

find_package(CURL REQUIRED)

add_library(registryfs_lib STATIC ${SOURCE_REGISTRYFS})
target_include_directories(registryfs_lib PUBLIC
    ${CURL_INCLUDE_DIRS}
    ${RAPIDJSON_INCLUDE_DIRS}
    ${PHOTON_INCLUDE_DIR}
)
