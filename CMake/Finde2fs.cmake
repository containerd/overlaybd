if(DEPENDENCY_E2FSPROGS_REPOSITORY)
    message("Add and build standalone libext2fs")
    include(FetchContent)

    FetchContent_Declare(e2fsprogs
        GIT_REPOSITORY ${DEPENDENCY_E2FSPROGS_REPOSITORY}
        GIT_TAG ${DEPENDENCY_E2FSPROGS_TAG}
    )
    FetchContent_GetProperties(e2fsprogs)

    if(NOT TARGET libext2fs_build)
        FetchContent_MakeAvailable(e2fsprogs)
        set(LIBEXT2FS_INSTALL_DIR ${e2fsprogs_SOURCE_DIR}/build/v1.47.0-opt CACHE STRING "")
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
    # E2FS_LIBRARIES is what photon's own build consumes, overlaybd links the
    # imported targets below instead.
    set(E2FS_LIBRARIES ${E2FS_LIBRARY} ${E2FS_COM_ERR_LIBRARY})
    set(E2FS_INCLUDE_DIR ${LIBEXT2FS_INSTALL_DIR}/include)
    set(E2FS_INCLUDE_DIRS ${E2FS_INCLUDE_DIR})

    # The libraries and headers only appear once libext2fs_build has run, but
    # imported targets validate INTERFACE_INCLUDE_DIRECTORIES at configure time.
    file(MAKE_DIRECTORY ${E2FS_INCLUDE_DIR})
else()
    find_path(E2FS_INCLUDE_DIRS ext2fs/ext2fs.h)
    find_library(E2FS_LIBRARY ext2fs)
    find_library(E2FS_COM_ERR_LIBRARY com_err)
    set(E2FS_LIBRARIES ${E2FS_LIBRARY})

    find_package_handle_standard_args(e2fs DEFAULT_MSG E2FS_LIBRARY
                                      E2FS_INCLUDE_DIRS)
endif()

if(E2FS_FOUND)
    if(E2FS_COM_ERR_LIBRARY AND NOT TARGET E2FSPROGS::libcom_err)
        add_library(E2FSPROGS::libcom_err UNKNOWN IMPORTED)
        set_target_properties(
            E2FSPROGS::libcom_err
            PROPERTIES IMPORTED_LINK_INTERFACE_LANGUAGES "C"
                       IMPORTED_LOCATION "${E2FS_COM_ERR_LIBRARY}")
        if(DEPENDENCY_E2FSPROGS_REPOSITORY)
            add_dependencies(E2FSPROGS::libcom_err libext2fs_build)
        endif()
    endif()

    if(NOT TARGET E2FSPROGS::libext2fs)
        add_library(E2FSPROGS::libext2fs UNKNOWN IMPORTED)
        set_target_properties(
            E2FSPROGS::libext2fs
            PROPERTIES IMPORTED_LINK_INTERFACE_LANGUAGES "C"
                       IMPORTED_LOCATION "${E2FS_LIBRARY}"
                       INTERFACE_INCLUDE_DIRECTORIES "${E2FS_INCLUDE_DIRS}")
        # libext2fs calls into com_err, which therefore has to follow it.
        if(TARGET E2FSPROGS::libcom_err)
            set_property(TARGET E2FSPROGS::libext2fs PROPERTY
                         INTERFACE_LINK_LIBRARIES E2FSPROGS::libcom_err)
        endif()
        if(DEPENDENCY_E2FSPROGS_REPOSITORY)
            add_dependencies(E2FSPROGS::libext2fs libext2fs_build)
        endif()
    endif()
endif()

mark_as_advanced(E2FS_INCLUDE_DIRS E2FS_LIBRARY E2FS_COM_ERR_LIBRARY)
