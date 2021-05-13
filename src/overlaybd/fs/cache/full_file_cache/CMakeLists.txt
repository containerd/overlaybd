file(GLOB SRC_FULLFILECACHE "*.cpp")

add_library(full_file_cache_lib STATIC ${SRC_FULLFILECACHE})

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
