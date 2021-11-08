file(GLOB SOURCE_FS "*.cpp")

add_library(fs_lib STATIC ${SOURCE_FS})

add_subdirectory(registryfs)
add_subdirectory(lsmt)
add_subdirectory(zfile)
add_subdirectory(cache)

target_link_libraries(fs_lib
    registryfs_lib
    lsmt_lib
    zfile_lib
    cache_lib
)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
