file(GLOB SOURCE_ZFILE "*.cpp") 
file(GLOB SOURCE_LZ4 "lz4/lz4.c")
file(GLOB SOURCE_CRC32 "crc32/*.cpp") 

add_library(zfile_lib STATIC ${SOURCE_ZFILE} ${SOURCE_LZ4} ${SOURCE_CRC32})
target_compile_options(zfile_lib PUBLIC -msse4.2 -mcrc32)
target_link_libraries(zfile_lib ${LIBRARY_OUTPUT_PATH}/libaccel-config.so -luuid)

add_custom_command(TARGET zfile_lib
    # run before any other rules are executed within the target.
    PRE_BUILD
    COMMAND echo -e "\texecuting a PRE_BUILD command"
    COMMAND git clone  https://github.com/intel/idxd-config
    COMMAND cd idxd-config && git checkout stable
    COMMAND cd idxd-config && ./autogen.sh
    COMMAND cd idxd-config && ./configure --prefix=/usr --sysconfdir=/etc --libdir=/usr/lib64
    COMMAND cd idxd-config && make && mv ./accfg/lib/.libs/libaccel-config.so ${LIBRARY_OUTPUT_PATH} 
    && cp ./accfg/lib/.libs/libaccel-config.so.1 ${LIBRARY_OUTPUT_PATH}
    && cp ./accfg/lib/.libs/libaccel-config.so.1.0.0 ${LIBRARY_OUTPUT_PATH}
    COMMENT "This command will be executed before building zfile_lib"
    VERBATIM # to support \t for example
)

add_custom_command(TARGET zfile_lib
    # Run after all other rules within the target have been executed
    POST_BUILD
    COMMAND rm -rf idxd-config/
    COMMAND echo -e "\texecuting a POST_BUILD command"
    COMMENT "This command will be executed after building zfile_lib"
    VERBATIM
)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
