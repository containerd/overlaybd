file(GLOB SOURCE_TAR "*.cpp")

add_library(untar_lib STATIC ${SOURCE_TAR})
target_include_directories(untar_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)

# if(BUILD_TESTING)
#  add_subdirectory(test)
# endif()
add_subdirectory(test2)