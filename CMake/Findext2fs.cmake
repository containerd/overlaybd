include(FetchContent)
set(FETCHCONTENT_QUIET false)

FetchContent_Declare(
  e2fsprogs
  GIT_REPOSITORY https://github.com/data-accelerator/e2fsprogs.git
  GIT_TAG b4d19a0ec5cb535f13009d910833c62aa70d69a5
)

FetchContent_MakeAvailable(e2fsprogs)

set(LIBEXT2FS_INSTALL_DIR ${e2fsprogs_SOURCE_DIR}/build/libext2fs)
add_custom_command(
  OUTPUT ${LIBEXT2FS_INSTALL_DIR}/lib
  WORKING_DIRECTORY ${e2fsprogs_SOURCE_DIR}
  COMMAND chmod 755 build.sh && ./build.sh
)
add_custom_target(libext2fs ALL DEPENDS ${LIBEXT2FS_INSTALL_DIR}/lib)

set(EXT2FS_INCLUDE_DIR ${LIBEXT2FS_INSTALL_DIR}/include)
set(EXT2FS_LIBRARY ${LIBEXT2FS_INSTALL_DIR}/lib/libext2fs.so)
