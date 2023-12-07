file(GLOB SOURCE_TAR "*.cpp")
add_library(gzindex_lib STATIC ${SOURCE_TAR})

target_include_directories(gzindex_lib PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(gzindex_lib photon_static)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
