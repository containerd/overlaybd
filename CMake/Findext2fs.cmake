include(FetchContent)
set(FETCHCONTENT_QUIET false)

FetchContent_Declare(
  ext2fs
  URL https://github.com/data-accelerator/e2fsprogs/releases/download/latest/libext2fs.tar.gz
)

FetchContent_GetProperties(ext2fs)
if (NOT ext2fs_POPULATED)
  FetchContent_Populate(ext2fs)
endif()

set(EXT2FS_INCLUDE_DIR ${ext2fs_SOURCE_DIR}/include)
find_library(EXT2FS_LIBRARY ext2fs HINTS ${ext2fs_SOURCE_DIR}/lib/)
