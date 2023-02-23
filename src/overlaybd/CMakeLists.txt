add_subdirectory(registryfs)
add_subdirectory(lsmt)
add_subdirectory(zfile)
add_subdirectory(cache)
add_subdirectory(untar)
add_subdirectory(extfs)
add_subdirectory(gzip)
add_subdirectory(gzindex)

file(GLOB SOURCE_OBD "*.cpp")
add_library(overlaybd_lib STATIC ${SOURCE_OBD})
target_include_directories(overlaybd_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)
target_link_libraries(overlaybd_lib
    photon_static
    registryfs_lib
    lsmt_lib
    zfile_lib
    cache_lib
    untar_lib
    extfs_lib
    gzip_lib
    gzindex_lib
)
