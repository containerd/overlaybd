file(GLOB SOURCE_ZSTD "*.cpp")

add_library(zstd_lib STATIC ${SOURCE_ZSTD})

target_include_directories(zstd_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)

include(FetchContent)

# Use FetchContent to download zstd
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
)

FetchContent_MakeAvailable(zstd)

target_link_libraries(zstd_lib photon_static zstd)
