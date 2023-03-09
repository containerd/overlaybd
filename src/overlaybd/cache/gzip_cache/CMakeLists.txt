file(GLOB SRC_FRONTEND "*.cpp")

add_library(gzip_cache_lib STATIC ${SRC_FRONTEND})
target_link_libraries(gzip_cache_lib gzindex_lib)
target_include_directories(gzip_cache_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)