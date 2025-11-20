file(GLOB SOURCE_ZSTD "*.cpp")

add_library(zstd_lib STATIC ${SOURCE_ZSTD})

target_include_directories(zstd_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)

target_link_libraries(zstd_lib photon_static ${LIBZSTD})
