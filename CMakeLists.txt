cmake_minimum_required(VERSION 3.14)

project(
    overlaybd
    LANGUAGES C CXX
)

#---------------------------------------------------------------------
# Options
#---------------------------------------------------------------------
option(FULL_STATIC "make all binary output depends only libc dynamic libs" on)
option(BUILD_CURL_FROM_SOURCE "Compile static libcurl" off)
option(BUILD_STREAM_CONVERTOR "Build the stream convertor" on)
option(ORIGIN_EXT2FS "Use original libext2fs" off)
option(ENABLE_OCF_CACHE "Build the OCF cache backend (requires the ocf submodule)" on)
option(ENABLE_QAT "Build the QAT compression backend" off)
option(ENABLE_DSA "Build the DSA crc32 backend (requires the DML submodule)" off)
option(ENABLE_ISAL "Build the ISA-L crc32 backend (requires the isa-l submodule)" off)

set(OBD_VER "overlaybd/0.0.0-undefined" CACHE STRING "Overlaybd version")

#---------------------------------------------------------------------
# Toolchain
#---------------------------------------------------------------------
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|aarch64|arm64)$")
    message(FATAL_ERROR "Unknown CPU architecture ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED on)
set(CMAKE_POSITION_INDEPENDENT_CODE on) # -fpic
set(CMAKE_EXPORT_COMPILE_COMMANDS on)

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -DNDEBUG -g")

add_compile_options(-Wall -Werror=sign-compare)
add_definitions(-DOVERLAYBD_VER=${OBD_VER})

# GCC 13+ (default on Azure Linux 4.0 with GCC 15) no longer transitively
# includes <cstdint> from other standard headers. Force-include it for every
# C++ translation unit so that fixed-width integer types (uint64_t, etc.) used
# in third-party headers (e.g. photon's fs/path.h at the pinned commit) resolve
# without needing to upgrade those pins. C sources (e.g. OCF) are unaffected.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include cstdint")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    # +crc also covers what crc32_lib below needs on this architecture
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+crc -fsigned-char\
 -fno-stack-protector -fomit-frame-pointer")
endif()

set(CMAKE_CXX_STANDARD_LIBRARIES "-static-libgcc ${CMAKE_CXX_STANDARD_LIBRARIES}")
find_library(STATIC_LIBSTDCXX libstdc++.a PATHS /usr/lib/gcc/*/*)
if(STATIC_LIBSTDCXX)
    set(CMAKE_CXX_STANDARD_LIBRARIES "-static-libstdc++ ${CMAKE_CXX_STANDARD_LIBRARIES}")
endif()

# Kept under the source tree: the CI jobs and the packaging scripts expect the
# binaries at build/output regardless of where cmake was invoked from.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/output")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/output")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/output")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")
include(FindPackageHandleStandardArgs)

# Prefer static archives for everything we link. Findtcmu.cmake temporarily
# restores the saved list, as libnl is only shipped as a shared library.
set(CMAKE_FIND_LIBRARY_SUFFIXES_SAVE ${CMAKE_FIND_LIBRARY_SUFFIXES})
if(FULL_STATIC)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif()

#---------------------------------------------------------------------
# Sources of the dependencies
#
# For each dependency, setting DEPENDENCY_<DEP>_REPOSITORY selects where to
# fetch and build it from, at DEPENDENCY_<DEP>_TAG. Setting the repository to
# empty selects the locally installed one instead, which is then looked up with
# the usual find_path()/find_library() (honoring CMAKE_PREFIX_PATH etc.), e.g.
#
#   cmake -DDEPENDENCY_PHOTON_REPOSITORY= -DCMAKE_PREFIX_PATH=/path/to/deps ...
#
# Note that a locally installed dependency is used as is: it is the caller's
# responsibility to make sure it is compatible, since the tag below is then
# neither used nor checked.
#---------------------------------------------------------------------
set(DEPENDENCY_PHOTON_REPOSITORY "https://github.com/alibaba/PhotonLibOS.git"
    CACHE STRING "repository of photon, empty to use the local installation")
set(DEPENDENCY_PHOTON_TAG "0178d14499d8639759a81e32ad58da5226df5e9b"
    CACHE STRING "tag/commit of photon")

set(DEPENDENCY_TCMU_REPOSITORY "https://github.com/data-accelerator/photon-libtcmu.git"
    CACHE STRING "repository of photon-libtcmu, empty to use the local installation")
set(DEPENDENCY_TCMU_TAG "813fd65361bb2f348726b9c41478a44211847614"
    CACHE STRING "tag/commit of photon-libtcmu")

set(DEPENDENCY_E2FSPROGS_REPOSITORY "https://github.com/data-accelerator/e2fsprogs.git"
    CACHE STRING "repository of e2fsprogs, empty to use the local installation")
set(DEPENDENCY_E2FSPROGS_TAG "404deb95e6b0ed0ceb0148d289977e65bee7f8d0"
    CACHE STRING "tag/commit of e2fsprogs")

set(DEPENDENCY_EROFS_UTILS_REPOSITORY "https://github.com/erofs/erofs-utils.git"
    CACHE STRING "repository of erofs-utils, empty to use the local installation")
set(DEPENDENCY_EROFS_UTILS_TAG "eec6f7a2755dfccc8f655aa37cf6f26db9164e60"
    CACHE STRING "tag/commit of erofs-utils")

set(DEPENDENCY_RAPIDJSON_REPOSITORY "https://github.com/Tencent/rapidjson.git"
    CACHE STRING "repository of rapidjson, empty to use the local installation")
set(DEPENDENCY_RAPIDJSON_TAG "80b6d1c83402a5785c486603c5611923159d0894"
    CACHE STRING "tag/commit of rapidjson")

# 0.9.0 includes the missing <cstdint> include (PR #1310) required to build with
# GCC 15 (C23 default), e.g. on Azure Linux 4.0.
set(DEPENDENCY_YAML_CPP_REPOSITORY "https://github.com/jbeder/yaml-cpp.git"
    CACHE STRING "repository of yaml-cpp, empty to use the local installation")
set(DEPENDENCY_YAML_CPP_TAG "yaml-cpp-0.9.0"
    CACHE STRING "tag/commit of yaml-cpp")

# curl and openssl default to the local installation, as building them from
# source is only needed for fully static binaries. BUILD_CURL_FROM_SOURCE below
# fills in the repositories known to work for that.
set(DEPENDENCY_CURL_REPOSITORY ""
    CACHE STRING "repository of curl, empty to use the local installation")
set(DEPENDENCY_CURL_TAG "curl-7_42_1"
    CACHE STRING "tag/commit of curl")

set(DEPENDENCY_OPENSSL_REPOSITORY ""
    CACHE STRING "repository of openssl, empty to use the local installation")
set(DEPENDENCY_OPENSSL_TAG "OpenSSL_1_0_2-stable"
    CACHE STRING "tag/commit of openssl")

# ORIGIN_EXT2FS and BUILD_CURL_FROM_SOURCE predate the variables above and are
# kept as shortcuts for them. They are also still consumed on their own, so keep
# both forms in sync.
if(ORIGIN_EXT2FS)
    set(DEPENDENCY_E2FSPROGS_REPOSITORY "")
elseif(NOT DEPENDENCY_E2FSPROGS_REPOSITORY)
    set(ORIGIN_EXT2FS on)
endif()
if(BUILD_CURL_FROM_SOURCE)
    if(NOT DEPENDENCY_CURL_REPOSITORY)
        set(DEPENDENCY_CURL_REPOSITORY "https://github.com/curl/curl.git")
    endif()
    if(NOT DEPENDENCY_OPENSSL_REPOSITORY)
        set(DEPENDENCY_OPENSSL_REPOSITORY "https://github.com/openssl/openssl.git")
    endif()
elseif(DEPENDENCY_CURL_REPOSITORY OR DEPENDENCY_OPENSSL_REPOSITORY)
    set(BUILD_CURL_FROM_SOURCE on)
endif()

#---------------------------------------------------------------------
# External dependencies
#---------------------------------------------------------------------
set(ENABLE_MIMIC_VDSO off) # photon option

# e2fs is looked up before photon: photon's own build looks it up as well, and
# photon_obj is ordered after the libext2fs build declared by this call.
find_package(e2fs REQUIRED)      # E2FSPROGS::libext2fs E2FSPROGS::libcom_err
find_package(photon REQUIRED)    # PHOTON::photon
find_package(tcmu REQUIRED)      # TCMU::tcmu
find_package(CURL REQUIRED)      # CURL::libcurl
find_package(OpenSSL REQUIRED)   # OpenSSL::SSL OpenSSL::Crypto
find_package(ZLIB REQUIRED)      # ZLIB::ZLIB
find_package(aio REQUIRED)       # AIO::aio
find_package(zstd REQUIRED)      # ZSTD::zstd
find_package(erofsutils REQUIRED) # EROFS::utils
find_package(RapidJSON REQUIRED) # (header only) RAPIDJSON_INCLUDE_DIRS
if(BUILD_STREAM_CONVERTOR)
    find_package(yamlcpp REQUIRED) # yaml-cpp
endif()

# Everything overlaybd links besides its own libraries, declared once here
# instead of being spelled out by every target.
#
# photon and overlaybd share these: photon references curl, openssl, zlib and
# libaio, and overlaybd uses openssl and zlib on its own as well. They are all
# static in the default configuration, so their relative order on the link line
# matters -- a single list is what keeps that order right, in particular
# libcurl.a needing openssl and zlib to follow it.
#
# Dependencies only some of the binaries need are deliberately not part of this,
# E2FSPROGS::libext2fs in particular: it is linked by the targets that go
# through photon's extfs support, so that the ones that do not are not tied to
# libext2fs.so at runtime.
add_library(external_lib INTERFACE)
target_include_directories(external_lib INTERFACE ${RAPIDJSON_INCLUDE_DIRS})
target_link_libraries(external_lib INTERFACE
    PHOTON::photon
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    AIO::aio
    pthread
    rt
    dl
    resolv
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # photon needs libgcc for the hidden __cpu_model symbol
    target_link_libraries(external_lib INTERFACE gcc)
endif()

#---------------------------------------------------------------------
# overlaybd libraries
#---------------------------------------------------------------------
add_library(checksum_lib STATIC src/tools/sha256file.cpp)
target_link_libraries(checksum_lib PUBLIC external_lib)

add_library(registryfs_lib STATIC
    src/overlaybd/registryfs/registryfs.cpp
    src/overlaybd/registryfs/registryfs_v2.cpp
)
target_link_libraries(registryfs_lib PUBLIC external_lib)

add_library(lsmt_lib STATIC
    src/overlaybd/lsmt/file.cpp
    src/overlaybd/lsmt/index.cpp
)
target_link_libraries(lsmt_lib PUBLIC external_lib)

# crc32c.cpp is the only source built for a specific ISA extension, hence its
# own library rather than compile options on zfile_lib as a whole.
add_library(crc32_lib STATIC src/overlaybd/zfile/crc32/crc32c.cpp)
target_link_libraries(crc32_lib PUBLIC external_lib)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64)
    target_compile_options(crc32_lib PUBLIC -msse4.2 -mcrc32)
endif()

add_library(zfile_lib STATIC
    src/overlaybd/zfile/compressor.cpp
    src/overlaybd/zfile/zfile.cpp
    src/overlaybd/zfile/lz4/lz4.c
)
target_link_libraries(zfile_lib PUBLIC external_lib crc32_lib ZSTD::zstd)

add_library(zstd_lib STATIC src/overlaybd/zstd/zstdfile.cpp)
target_link_libraries(zstd_lib PUBLIC external_lib ZSTD::zstd)

add_library(gzip_lib STATIC src/overlaybd/gzip/gz.cpp)
target_link_libraries(gzip_lib PUBLIC external_lib checksum_lib)

add_library(gzindex_lib STATIC
    src/overlaybd/gzindex/gzfile.cpp
    src/overlaybd/gzindex/gzip_index_create.cpp
)
target_link_libraries(gzindex_lib PUBLIC external_lib)

add_library(cache_lib STATIC
    src/overlaybd/cache/cache.cpp
    src/overlaybd/cache/cached_fs.cpp
    src/overlaybd/cache/store.cpp
    src/overlaybd/cache/full_file_cache/cache_pool.cpp
    src/overlaybd/cache/full_file_cache/cache_store.cpp
    src/overlaybd/cache/download_cache/download_cache.cpp
    src/overlaybd/cache/gzip_cache/cached_fs.cpp
)
target_link_libraries(cache_lib PUBLIC external_lib gzindex_lib)

# EROFS::utils propagates its include dir and the "-include config.h" compile
# option, and orders erofs_lib after the erofs-utils build
add_library(erofs_lib STATIC
    src/overlaybd/tar/erofs/erofs_common.cpp
    src/overlaybd/tar/erofs/erofs_fs.cpp
    src/overlaybd/tar/erofs/liberofs.cpp
)
target_link_libraries(erofs_lib PUBLIC external_lib PRIVATE EROFS::utils)

add_library(tar_lib STATIC
    src/overlaybd/tar/header.cpp
    src/overlaybd/tar/libtar.cpp
    src/overlaybd/tar/tar_file.cpp
    src/overlaybd/tar/whiteout.cpp
)
target_link_libraries(tar_lib PUBLIC external_lib PRIVATE erofs_lib)

#---------------------------------------------------------------------
# Optional acceleration backends
#---------------------------------------------------------------------
if(ENABLE_QAT)
    find_path(QAT_INCLUDE_DIR NAMES qat/cpa.h PATHS /usr/include/qat /usr/local/include/qat)
    find_library(QAT_LIBRARY NAMES qat)
    find_library(USDM_LIBRARY NAMES usdm)
    if(NOT (QAT_INCLUDE_DIR AND QAT_LIBRARY AND USDM_LIBRARY))
        message(FATAL_ERROR "ENABLE_QAT=on but the QAT headers/libraries were not found")
    endif()
    target_sources(zfile_lib PRIVATE src/overlaybd/zfile/lz4/lz4-qat.cpp)
    target_compile_definitions(zfile_lib PUBLIC ENABLE_QAT)
    target_include_directories(zfile_lib PUBLIC ${QAT_INCLUDE_DIR})
    target_link_libraries(zfile_lib PUBLIC ${QAT_LIBRARY} ${USDM_LIBRARY} pci)
endif()

# DML and isa-l bring their own autotools/cmake builds, driven here at build
# time and consumed from the output directory they are told to install into.
if(ENABLE_DSA OR ENABLE_ISAL)
    set(THIRDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/overlaybd/zfile/thirdparty")
    add_custom_target(thirdparty_lib)
    target_link_directories(crc32_lib PUBLIC ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
    target_include_directories(crc32_lib PUBLIC ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/include)
    add_dependencies(crc32_lib thirdparty_lib)
endif()
if(ENABLE_DSA)
    add_custom_command(TARGET thirdparty_lib
        WORKING_DIRECTORY ${THIRDPARTY_DIR}/DML
        COMMAND git checkout 5a2956362d7b1c65d8aee753938ae9bf5cf0b7fd
        COMMAND rm -rf build && mkdir build
        COMMAND cd build && cmake -DCMAKE_INSTALL_PREFIX=dmldir .. && cmake --build . --target install
        COMMAND cd build/dmldir && find ./ -name "libdml*.a" | xargs -i cp {} ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/
        COMMAND cp -r build/dmldir/include ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/
    )
    target_compile_definitions(crc32_lib PUBLIC ENABLE_DSA)
    target_link_libraries(crc32_lib PUBLIC pci dmlhl)
endif()
if(ENABLE_ISAL)
    add_custom_command(TARGET thirdparty_lib
        WORKING_DIRECTORY ${THIRDPARTY_DIR}/isa-l
        COMMAND git checkout ad8dce15c6d3f0c7f3d1b486d9c649ed39223b45
        COMMAND ./autogen.sh && ./configure && make
        COMMAND cp .libs/libisal.a ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/
        COMMAND mkdir -p ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/include
        COMMAND cp include/crc.h ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/include/
    )
    target_compile_definitions(crc32_lib PUBLIC ENABLE_ISAL)
    target_compile_options(crc32_lib PUBLIC -mavx512f)
    target_link_libraries(crc32_lib PUBLIC isal)
endif()

if(ENABLE_OCF_CACHE)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/overlaybd/cache/ocf_cache/ocf/src")
        message(FATAL_ERROR "the ocf submodule is missing, please run "
            "'git submodule update --init' first, or configure with "
            "-DENABLE_OCF_CACHE=off to disable the OCF cache backend")
    endif()
    set(OCF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/overlaybd/cache/ocf_cache")

    add_library(ocf_env_lib STATIC
        ${OCF_DIR}/ease_bindings/env/ocf_env.cpp
        ${OCF_DIR}/ease_bindings/env/utils_mpool.cpp
    )
    target_include_directories(ocf_env_lib PUBLIC ${OCF_DIR}/include)
    target_link_libraries(ocf_env_lib PUBLIC external_lib)

    # the vendored OCF sources are globbed: they are not ours to enumerate
    file(GLOB_RECURSE src_ocf ${OCF_DIR}/ocf/src/*.c)
    add_library(ocf_lib STATIC ${src_ocf})
    target_include_directories(ocf_lib PUBLIC ${OCF_DIR}/include ${OCF_DIR}/ease_bindings/env)
    target_link_libraries(ocf_lib PUBLIC ocf_env_lib ZLIB::ZLIB)
    target_compile_options(ocf_lib PRIVATE -Wno-sign-compare)

    add_library(ocf_cache_lib STATIC
        ${OCF_DIR}/ocf_cache.cpp
        ${OCF_DIR}/ocf_namespace.cpp
        ${OCF_DIR}/ease_bindings/ctx.cpp
        ${OCF_DIR}/ease_bindings/provider.cpp
        ${OCF_DIR}/ease_bindings/queue.cpp
        ${OCF_DIR}/ease_bindings/volume.cpp
    )
    target_include_directories(ocf_cache_lib PUBLIC ${OCF_DIR}/ease_bindings/env)
    target_link_libraries(ocf_cache_lib PUBLIC external_lib ocf_lib)

    target_link_libraries(cache_lib PUBLIC ocf_cache_lib)
endif()

# overlaybd_lib - the aggregate every binary links against
add_library(overlaybd_lib INTERFACE)
target_link_libraries(overlaybd_lib INTERFACE
    external_lib
    registryfs_lib
    lsmt_lib
    zfile_lib
    zstd_lib
    cache_lib
    tar_lib
    gzip_lib
    gzindex_lib
)

add_library(overlaybd_image_lib STATIC
    src/image_file.cpp
    src/image_service.cpp
    src/switch_file.cpp
    src/bk_download.cpp
    src/prefetch.cpp
    src/api_server.cpp
    src/tools/comm_func.cpp
)
target_link_libraries(overlaybd_image_lib PUBLIC
    overlaybd_lib
    checksum_lib
    # image_file.cpp, prefetch.cpp and comm_func.cpp go through photon's extfs
    E2FSPROGS::libext2fs
)
if(ENABLE_OCF_CACHE)
    target_compile_definitions(overlaybd_image_lib PRIVATE ENABLE_OCF_CACHE)
endif()

# Propagate the resize .o files to all consumers of overlaybd_image_lib. These
# provide the resize_fs() symbol needed by photon's resize_extfs().
if(NOT ORIGIN_EXT2FS)
    set_source_files_properties(
        ${E2FS_RESIZE_DIR}/resize2fs.o
        ${E2FS_RESIZE_DIR}/extent.o
        ${E2FS_RESIZE_DIR}/resource_track.o
        PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    target_link_libraries(overlaybd_image_lib PUBLIC
        ${E2FS_RESIZE_DIR}/resize2fs.o
        ${E2FS_RESIZE_DIR}/extent.o
        ${E2FS_RESIZE_DIR}/resource_track.o
    )
    add_dependencies(overlaybd_image_lib libext2fs_build)
endif()

#---------------------------------------------------------------------
# Executables
#---------------------------------------------------------------------
add_executable(overlaybd-tcmu src/main.cpp)
target_include_directories(overlaybd-tcmu PRIVATE ${TCMU_INCLUDE_DIR})
target_link_libraries(overlaybd-tcmu overlaybd_image_lib TCMU::tcmu)

add_executable(overlaybd-commit src/tools/overlaybd-commit.cpp)
target_link_libraries(overlaybd-commit overlaybd_image_lib)

add_executable(overlaybd-merge src/tools/overlaybd-merge.cpp)
target_link_libraries(overlaybd-merge overlaybd_image_lib)

# overlaybd-create.cpp goes through photon's extfs, the other tools reach it
# through overlaybd_image_lib
add_executable(overlaybd-create src/tools/overlaybd-create.cpp)
target_link_libraries(overlaybd-create overlaybd_lib E2FSPROGS::libext2fs)

add_executable(overlaybd-zfile src/tools/overlaybd-zfile.cpp)
target_link_libraries(overlaybd-zfile overlaybd_lib)

add_executable(overlaybd-apply src/tools/overlaybd-apply.cpp src/tools/qcow2converter.cpp)
target_link_libraries(overlaybd-apply overlaybd_image_lib)

add_executable(turboOCI-apply src/tools/turboOCI-apply.cpp)
target_link_libraries(turboOCI-apply overlaybd_image_lib)

set(OVERLAYBD_TOOLS
    overlaybd-commit
    overlaybd-merge
    overlaybd-create
    overlaybd-zfile
    overlaybd-apply
    turboOCI-apply
)

# overlaybd-resize: userspace ext4 resize for overlaybd images, only available
# with the libext2fs built here
if(NOT ORIGIN_EXT2FS)
    add_executable(overlaybd-resize src/tools/overlaybd-resize.cpp)
    target_link_libraries(overlaybd-resize overlaybd_image_lib)
    list(APPEND OVERLAYBD_TOOLS overlaybd-resize)
endif()

if(BUILD_STREAM_CONVERTOR)
    add_executable(overlaybd-streamConv src/overlaybd/stream_convertor/stream_conv.cpp)
    target_link_libraries(overlaybd-streamConv
        external_lib tar_lib gzip_lib gzindex_lib yaml-cpp)
endif()

# The tools are installed next to the libraries they may load at runtime.
set_target_properties(${OVERLAYBD_TOOLS} PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib:$ORIGIN/../lib64")

#---------------------------------------------------------------------
# baselayer - the empty ext4 image the tools start from
#---------------------------------------------------------------------
add_custom_command(
    OUTPUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ext4_64
    COMMAND tar -zxvf ${CMAKE_CURRENT_SOURCE_DIR}/baselayers/ext4_64.tar.gz
                -C ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    VERBATIM
)
add_custom_target(baselayer ALL DEPENDS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ext4_64)

#---------------------------------------------------------------------
# Install
#---------------------------------------------------------------------
install(TARGETS overlaybd-tcmu DESTINATION /opt/overlaybd/bin)
install(TARGETS ${OVERLAYBD_TOOLS} DESTINATION /opt/overlaybd/bin)
install(FILES src/example_config/overlaybd-tcmu.service DESTINATION /opt/overlaybd/)
install(FILES src/example_config/cred.json DESTINATION /opt/overlaybd/)
install(FILES src/example_config/overlaybd.json DESTINATION /etc/overlaybd/)
install(FILES ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ext4_64 DESTINATION /opt/overlaybd/baselayers)
if(NOT ORIGIN_EXT2FS)
    install(DIRECTORY ${LIBEXT2FS_INSTALL_DIR}/lib DESTINATION /opt/overlaybd/ USE_SOURCE_PERMISSIONS)
endif()

include(pack)

#---------------------------------------------------------------------
# Unit tests
#---------------------------------------------------------------------
if(BUILD_TESTING)
    enable_testing()
    include(CTest)
    add_subdirectory(test)
endif()
