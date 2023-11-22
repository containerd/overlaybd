add_subdirectory(full_file_cache)
add_subdirectory(ocf_cache)
add_subdirectory(download_cache)
add_subdirectory(gzip_cache)

file(GLOB SRC_CACHE "*.cpp")

add_library(cache_lib STATIC ${SRC_CACHE})
target_link_libraries(cache_lib
    photon_static
    full_file_cache_lib
    ocf_cache_lib
    download_cache_lib
    gzip_cache_lib
)
target_include_directories(cache_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
