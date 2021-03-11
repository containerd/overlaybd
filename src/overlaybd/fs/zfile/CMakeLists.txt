file(GLOB SOURCE_ZFILE "*.cpp") 
file(GLOB SOURCE_LZ4 "lz4/lz4.c")
file(GLOB SOURCE_CRC32 "crc32/*.cpp") 

add_library(zfile_lib STATIC ${SOURCE_ZFILE} ${SOURCE_LZ4} ${SOURCE_CRC32})
target_compile_options(zfile_lib PUBLIC -msse4.2 -mcrc32)
target_link_libraries(zfile_lib)

if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(test)
endif()