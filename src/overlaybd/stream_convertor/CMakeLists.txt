file(GLOB SOURCE_SERV "*.cpp")

add_executable(stream_conv ${SOURCE_SERV})
target_include_directories(stream_conv PUBLIC
    ${PHOTON_INCLUDE_DIR}
)
target_link_libraries(stream_conv
photon_static
gzip_lib
gzindex_lib
tar_lib
)

# if(BUILD_TESTING)
#   add_subdirectory(test)
# endif()
