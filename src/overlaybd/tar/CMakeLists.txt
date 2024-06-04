file(GLOB SOURCE_TAR "*.cpp")

add_library(tar_lib STATIC ${SOURCE_TAR})
target_include_directories(tar_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()

add_subdirectory(erofs)
target_link_libraries(tar_lib PRIVATE erofs_lib)