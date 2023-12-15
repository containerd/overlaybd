add_subdirectory(registryfs)
add_subdirectory(lsmt)
add_subdirectory(zfile)
add_subdirectory(cache)
add_subdirectory(tar)
add_subdirectory(gzip)
add_subdirectory(gzindex)
add_subdirectory(stream_convertor)

add_library(overlaybd_lib INTERFACE)
target_include_directories(overlaybd_lib INTERFACE
    ${PHOTON_INCLUDE_DIR}
)
target_link_libraries(overlaybd_lib INTERFACE
    photon_static
    registryfs_lib
    lsmt_lib
    zfile_lib
    cache_lib
    tar_lib
    gzip_lib
    gzindex_lib
)
