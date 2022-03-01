file(GLOB SOURCE_ZFILE "*.cpp")
file(GLOB SOURCE_LZ4 "lz4/lz4.c")
file(GLOB SOURCE_CRC32 "crc32/crc32c.cpp")

set(CHECK_DSA_CMD "if lspci -nn | grep 8086:0b25 > /dev/null \; then echo -n yes\; else echo -n no\; fi")
execute_process(COMMAND bash -c ${CHECK_DSA_CMD} OUTPUT_VARIABLE HAS_DSA)

if (${HAS_DSA} STREQUAL "yes")
    set (CMAKE_CXX_STANDARD 17)
    add_definitions(-DENABLE_DSA=yes)
    add_custom_command(OUTPUT temp
        PRE_BUILD
        COMMENT "Found Intel DSA device, going to build DML"
        COMMAND rm -rf DML/
        COMMAND git clone --recursive https://github.com/intel/DML.git
        COMMAND cd DML && mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -DDML_HW=ON ..
        COMMAND cd DML && cd build && cmake --build . --target install
        VERBATIM
    )
    add_library(zfile_lib STATIC temp ${SOURCE_ZFILE} ${SOURCE_LZ4} ${SOURCE_CRC32})
    target_link_libraries(zfile_lib -ldmlhl -ldl)
    set (CMAKE_CXX_STANDARD 14)
else ()
    add_library(zfile_lib STATIC ${SOURCE_ZFILE} ${SOURCE_LZ4} ${SOURCE_CRC32})
endif ()

target_compile_options(zfile_lib PUBLIC -msse4.2 -mcrc32)

if (BUILD_TESTING)
    add_subdirectory(test)
endif ()
