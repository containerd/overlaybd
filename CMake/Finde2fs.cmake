if(NOT ORIGIN_EXT2FS)
    message("Add and build standalone libext2fs")
    include(FetchContent)
    FetchContent_Declare(
        e2fsprogs
        GIT_REPOSITORY https://github.com/data-accelerator/e2fsprogs.git
        GIT_TAG 404deb95e6b0ed0ceb0148d289977e65bee7f8d0
    )
    FetchContent_GetProperties(e2fsprogs)

    if(NOT TARGET libext2fs_build)
        FetchContent_MakeAvailable(e2fsprogs)
        set(LIBEXT2FS_INSTALL_DIR ${e2fsprogs_SOURCE_DIR}/build/libext2fs CACHE STRING "")
        set(E2FS_RESIZE_DIR ${e2fsprogs_SOURCE_DIR}/build/resize CACHE STRING "path to e2fsprogs resize build dir")
        set(E2FS_INSTALL_LIB_DIR ${LIBEXT2FS_INSTALL_DIR}/lib CACHE STRING "path to e2fsprogs install-libs output")

        # Force the e2fsprogs autotools build to use C11. GCC 15 (Azure Linux
        # 4.0) defaults to C23, under which `typedef int bool;` in
        # lib/ext2fs/tdb.c is illegal because `bool` is now a keyword. The
        # upstream build.sh hardcodes `CFLAGS="-fPIC -O3"` on the configure
        # line, so setting CFLAGS via the environment is ignored -- we patch
        # build.sh in place instead. `-std=gnu11` is supported by every
        # compiler used across the release matrix (GCC 7+).
        add_custom_command(
            OUTPUT ${LIBEXT2FS_INSTALL_DIR}/lib
            WORKING_DIRECTORY ${e2fsprogs_SOURCE_DIR}
            COMMAND chmod 755 build.sh && sed -i 's|CFLAGS="-fPIC -O3"|CFLAGS="-fPIC -O3 -std=gnu11"|' build.sh && ./build.sh
        )
        add_custom_target(libext2fs_build DEPENDS ${LIBEXT2FS_INSTALL_DIR}/lib)
    endif()

    set(E2FS_FOUND yes)
    set(E2FS_LIBRARY ${LIBEXT2FS_INSTALL_DIR}/lib/libext2fs.so)
    set(E2FS_COM_ERR_LIBRARY ${e2fsprogs_SOURCE_DIR}/build/lib/libcom_err.a)
    set(E2FS_LIBRARIES ${E2FS_LIBRARY} ${E2FS_COM_ERR_LIBRARY})
    set(E2FS_INCLUDE_DIR ${LIBEXT2FS_INSTALL_DIR}/include)
    set(E2FS_INCLUDE_DIRS ${E2FS_INCLUDE_DIR})

    if(NOT TARGET libext2fs)
        add_library(libext2fs UNKNOWN IMPORTED)
    endif()
    add_dependencies(libext2fs libext2fs_build)

else()
    find_path(E2FS_INCLUDE_DIRS ext2fs/ext2fs.h)
    find_library(E2FS_LIBRARIES ext2fs)
endif()

find_package_handle_standard_args(e2fs DEFAULT_MSG E2FS_LIBRARIES E2FS_INCLUDE_DIRS)

mark_as_advanced(E2FS_INCLUDE_DIRS E2FS_LIBRARIES)
