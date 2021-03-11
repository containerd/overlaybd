file(GLOB SOURCE_FS "*.cpp") 

add_library(fs_lib STATIC ${SOURCE_FS})

add_subdirectory(registryfs)
add_subdirectory(lsmt)
add_subdirectory(zfile)
add_subdirectory(cache)

add_dependencies(fs_lib Boost)
target_include_directories(fs_lib PUBLIC 
    ${BOOST_INCLUDE_DIR}
)
target_link_libraries(fs_lib
    registryfs_lib
    lsmt_lib
    zfile_lib
    cache_lib
)