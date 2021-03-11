file(GLOB SRC_CACHE "*.cpp") 
add_library(cache_lib STATIC ${SRC_CACHE})

add_subdirectory(frontend)
add_subdirectory(full_file_cache)

target_link_libraries(cache_lib 
    cache_frontend_lib
    full_file_cache_lib
)