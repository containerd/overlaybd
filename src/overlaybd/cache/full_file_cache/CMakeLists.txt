file(GLOB SRC_FULLFILECACHE "*.cpp")

add_library(full_file_cache_lib STATIC ${SRC_FULLFILECACHE})
target_include_directories(full_file_cache_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)
