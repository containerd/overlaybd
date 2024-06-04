include(FetchContent)

FetchContent_Declare(
    erofs-utils
    GIT_REPOSITORY https://github.com/salvete/erofs-utils.git
    GIT_TAG      overlaybd-incremental-io
)

FetchContent_MakeAvailable(erofs-utils)

execute_process(
    COMMAND ./autogen.sh
    WORKING_DIRECTORY ${erofs-utils_SOURCE_DIR}
)
execute_process(
    COMMAND ./configure --disable-lz4 --without-liblzma
    WORKING_DIRECTORY ${erofs-utils_SOURCE_DIR}
)
execute_process(
    COMMAND make
    WORKING_DIRECTORY ${erofs-utils_SOURCE_DIR}
)

set(EROFS_LIB_INCLUDE_DIR "${erofs-utils_SOURCE_DIR}/include/" CACHE PATH "erofs-utils include path.")
set(EROFS_CONFIG_FILE "${erofs-utils_SOURCE_DIR}/config.h" CACHE PATH "erofs-utils config file.")
set(EROFS_LIB_STATIC "${erofs-utils_SOURCE_DIR}/lib/.libs/liberofs.a" CACHE PATH "erofs-utils static lib.")

file(GLOB EROFS_SOURCE "*.cpp")

add_library(erofs_lib STATIC ${EROFS_SOURCE})

target_include_directories(erofs_lib PRIVATE
    ${PHOTON_INCLUDE_DIR}
)

target_include_directories(erofs_lib PRIVATE
    ${EROFS_LIB_INCLUDE_DIR}
)

target_link_libraries(erofs_lib PRIVATE uuid)

target_compile_options(erofs_lib PRIVATE "-include${EROFS_CONFIG_FILE}")
target_link_libraries(erofs_lib PRIVATE ${EROFS_LIB_STATIC})