file(GLOB SRC_DOWNLOADCACHE "*.cpp")

add_library(download_cache_lib STATIC ${SRC_DOWNLOADCACHE})
target_include_directories(download_cache_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)
