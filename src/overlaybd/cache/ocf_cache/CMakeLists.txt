if(BUILD_TESTING)
  add_subdirectory(test)
endif()

# ocf_env_lib
file(GLOB_RECURSE src_ocf_env ease_bindings/env/*.cpp)
add_library(ocf_env_lib STATIC ${src_ocf_env})
target_include_directories(ocf_env_lib PUBLIC include/ ${PHOTON_INCLUDE_DIR})

# ocf_lib
file(GLOB_RECURSE src_ocf ocf/src/*.c)
add_library(ocf_lib STATIC ${src_ocf})
target_include_directories(ocf_lib PUBLIC include/ ease_bindings/env/)
target_link_libraries(ocf_lib ocf_env_lib z)
target_compile_options(ocf_lib PRIVATE -Wno-sign-compare)

# ocf_cache_lib
file(GLOB src_ocf_cache ocf_cache.cpp ocf_namespace.cpp ease_bindings/*.cpp)
add_library(ocf_cache_lib STATIC ${src_ocf_cache})
target_include_directories(ocf_cache_lib PUBLIC include/ ease_bindings/env/ ${PHOTON_INCLUDE_DIR})
target_link_libraries(ocf_cache_lib ocf_lib photon_static)