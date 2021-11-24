file(GLOB SOURCE_ZFILE "*.cpp")
file(GLOB SOURCE_LZ4 "lz4/lz4.c")
file(GLOB SOURCE_CRC32 "crc32/*.cpp")

add_library(zfile_lib STATIC ${SOURCE_ZFILE} ${SOURCE_LZ4} ${SOURCE_CRC32})
target_compile_options(zfile_lib PUBLIC -msse4.2 -mcrc32)

set(CHECK_DSA_CMD "if lspci -nn | grep 8086:0b25 > /dev/null \; then echo -n yes\; else echo -n no\; fi")
execute_process(COMMAND bash -c ${CHECK_DSA_CMD} OUTPUT_VARIABLE HAS_DSA)

if (${HAS_DSA} STREQUAL "yes")
    add_definitions(-DENABLE_DSA=yes)
    target_link_libraries(zfile_lib ${LIBRARY_OUTPUT_PATH}/libaccel-config.so -luuid)
    add_custom_command(TARGET zfile_lib
            PRE_BUILD
            COMMENT "Found Intel DSA device, going to build libaccel-config"
            COMMAND rm -rf idxd-config/
            COMMAND git clone https://github.com/intel/idxd-config && cd idxd-config && git checkout stable
            COMMAND cd idxd-config && ./autogen.sh && ./configure --disable-docs --disable-logging
            COMMAND cd idxd-config && make -j
            COMMAND cd idxd-config && cp -P ./accfg/lib/.libs/libaccel-config.so ${LIBRARY_OUTPUT_PATH} &&
                cp -P ./accfg/lib/.libs/libaccel-config.so.1 ${LIBRARY_OUTPUT_PATH} &&
                cp ./accfg/lib/.libs/libaccel-config.so.1.0.0 ${LIBRARY_OUTPUT_PATH}
            VERBATIM
            )
endif ()

if (BUILD_TESTING)
    add_subdirectory(test)
endif ()
