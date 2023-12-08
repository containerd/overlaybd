file(GLOB SOURCE_SERV "*.cpp")

add_executable(overlaybd-streamConv ${SOURCE_SERV})
target_include_directories(overlaybd-streamConv PUBLIC
    ${PHOTON_INCLUDE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)
target_link_libraries(overlaybd-streamConv
    photon_static
    gzip_lib
    gzindex_lib
    tar_lib
    yaml-cpp
)

# if(BUILD_TESTING)
#   add_subdirectory(test)
# endif()
