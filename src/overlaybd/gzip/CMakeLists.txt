file(GLOB SOURCE_GZIP "*.cpp")

add_library(gzip_lib STATIC ${SOURCE_GZIP})
target_include_directories(gzip_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)
target_link_libraries(gzip_lib photon_static checksum_lib)

# if(BUILD_TESTING)
#   add_subdirectory(test)
# endif()
