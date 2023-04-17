file(GLOB SOURCE_ZFILE "*.cpp")
file(GLOB SOURCE_LZ4 "lz4/*.c")
file(GLOB SOURCE_CRC32 "crc32/crc32c.cpp")

# Get CPU arch
execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT (${ARCH} STREQUAL x86_64) AND NOT (${ARCH} STREQUAL aarch64))
    message(FATAL_ERROR "Unknown CPU architecture ${ARCH}")
endif ()

set (CMAKE_CXX_STANDARD 17)
add_library(crc32_lib STATIC ${SOURCE_CRC32})
target_include_directories(crc32_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)

if (${ARCH} STREQUAL x86_64)
    target_compile_options(crc32_lib PUBLIC -msse4.2 -mcrc32)
else()
    target_compile_options(crc32_lib PUBLIC -march=native -mcpu=generic+crc)
endif()

if(ENABLE_DSA OR ENABLE_ISAL)
    add_subdirectory(thirdparty)
    add_dependencies(crc32_lib thirdparty_lib)
    target_link_directories(crc32_lib PUBLIC ${LIBRARY_OUTPUT_PATH})
    target_include_directories(crc32_lib PUBLIC ${LIBRARY_OUTPUT_PATH}/include)
    if(ENABLE_DSA)
        target_link_libraries(crc32_lib -lpci -ldmlhl -ldl)
        target_compile_definitions(crc32_lib PUBLIC -DENABLE_DSA)
    endif()
    if(ENABLE_ISAL)
        target_compile_options(crc32_lib PUBLIC -mavx512f)
        target_compile_definitions(crc32_lib PUBLIC -DENABLE_ISAL)
        target_link_libraries(crc32_lib -lisal)
    endif()
endif()
set (CMAKE_CXX_STANDARD 14)

add_library(zfile_lib STATIC ${SOURCE_ZFILE} ${SOURCE_LZ4})
target_link_libraries(zfile_lib photon_static crc32_lib ${LIBZSTD})

if (ENABLE_QAT)
    target_compile_definitions(zfile_lib PUBLIC -DENABLE_QAT)
    target_link_libraries(zfile_lib -lpthread -lpci)
    #target_link_libraries(zfile_lib -lqat_s -lusdm_drv_s -lpthread -lpci)
endif()

if (BUILD_TESTING)
    add_subdirectory(test)
endif ()
