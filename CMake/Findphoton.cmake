include(FetchContent)
include(FindPackageHandleStandardArgs)
set(FETCHCONTENT_QUIET false)

# PHOTON_ENABLE_RESIZE is also consumed by our own sources, so define it
# regardless of where photon comes from. A locally installed photon is then
# expected to provide extfs and resize as well.
set(PHOTON_ENABLE_EXTFS ON)

if(NOT ORIGIN_EXT2FS)
  set(PHOTON_ENABLE_RESIZE ON)
  add_definitions(-DPHOTON_ENABLE_RESIZE)
endif()

# The OCF cache backend is part of photon, and so is the OCF checkout it needs.
if(ENABLE_OCF_CACHE)
  set(PHOTON_BUILD_OCF_CACHE ON)
endif()

if(DEPENDENCY_PHOTON_REPOSITORY)
  FetchContent_Declare(photon
    GIT_REPOSITORY ${DEPENDENCY_PHOTON_REPOSITORY}
    GIT_TAG ${DEPENDENCY_PHOTON_TAG}
  )

  if(BUILD_TESTING)
    set(BUILD_TESTING 0)
    FetchContent_MakeAvailable(photon)
    set(BUILD_TESTING 1)
  else()
    FetchContent_MakeAvailable(photon)
  endif()

  # overlaybd links the archives below, libphoton.so is not consumed at all.
  # Keep it out of the default target: under FULL_STATIC its dependencies are
  # looked up as archives, which the system copies are commonly not built as
  # PIC and hence unusable in a shared library.
  if(TARGET photon_shared)
    set_target_properties(photon_shared PROPERTIES EXCLUDE_FROM_ALL on)
  endif()

  # photon_obj is only defined by photon's own build
  if (BUILD_CURL_FROM_SOURCE)
    find_package(OpenSSL REQUIRED)
    find_package(CURL REQUIRED)
    add_dependencies(photon_obj CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
  endif()

  if(NOT ORIGIN_EXT2FS)
    add_dependencies(photon_obj libext2fs_build)
  endif()

  if(ENABLE_OCF_CACHE)
    # photon's ocf targets include <ocf/...>, which its own build resolves
    # through a symlink pointing inside photon's build tree. That layout only
    # exists when photon is built standalone, so provide the same include
    # layout for the OCF checkout fetched as part of this build.
    FetchContent_GetProperties(ocf_lib SOURCE_DIR OCF_SOURCE_DIR)
    set(OCF_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/ocf-include)
    file(MAKE_DIRECTORY ${OCF_INCLUDE_DIR})
    file(CREATE_LINK ${OCF_SOURCE_DIR}/inc ${OCF_INCLUDE_DIR}/ocf SYMBOLIC)
    target_include_directories(ocf_lib PUBLIC ${OCF_INCLUDE_DIR})
    target_include_directories(ocf_cache_lib PUBLIC ${OCF_INCLUDE_DIR})
  endif()

  # Consume photon as plain archives rather than through its photon_static
  # target: the latter also propagates photon's own third-party dependencies,
  # whose declaration (which libraries, in which order) is an implementation
  # detail that has changed between photon versions. overlaybd brings those
  # dependencies itself, see the external_lib target in the top-level
  # CMakeLists.txt.
  set(PHOTON_INCLUDE_DIR ${photon_SOURCE_DIR}/include/)
  set(PHOTON_LIBRARY ${photon_BINARY_DIR}/output/libphoton_sole.a)
  set(PHOTON_EASY_WEAK_LIBRARY ${photon_BINARY_DIR}/output/libeasy_weak.a)
  if(ENABLE_OCF_CACHE)
    set(PHOTON_OCF_LIBRARY ${photon_BINARY_DIR}/output/libocf_cache_lib.a)
    set(PHOTON_OCF_CORE_LIBRARY ${photon_BINARY_DIR}/output/libocf_lib.a)
  endif()
  set(photon_FOUND yes)
else()
  # A locally installed photon has no photon_BINARY_DIR, look the archives up
  # instead. photon_sole is photon's own archive and the one paired with the
  # separate weak-symbol archives; libphoton.a is the merged one, which already
  # bundles them and is used as a fallback.
  find_path(PHOTON_INCLUDE_DIR photon/photon.h)
  find_library(PHOTON_LIBRARY NAMES photon_sole photon)
  find_library(PHOTON_EASY_WEAK_LIBRARY NAMES easy_weak)
  if(ENABLE_OCF_CACHE)
    find_library(PHOTON_OCF_LIBRARY NAMES ocf_cache_lib)
    find_library(PHOTON_OCF_CORE_LIBRARY NAMES ocf_lib)
  endif()

  find_package_handle_standard_args(photon DEFAULT_MSG PHOTON_LIBRARY
                                    PHOTON_INCLUDE_DIR)
endif()

if(photon_FOUND)
  if(NOT TARGET PHOTON::photonlib)
    add_library(PHOTON::photonlib STATIC IMPORTED)
    set_target_properties(
      PHOTON::photonlib
      PROPERTIES IMPORTED_LOCATION "${PHOTON_LIBRARY}"
                 INTERFACE_INCLUDE_DIRECTORIES "${PHOTON_INCLUDE_DIR}")
  endif()

  # The OCF cache backend is shipped as archives of its own rather than being
  # merged into photon's, and comes first on the link line: it is what provides
  # new_ocf_cached_fs(), and it needs photon in turn. libocf_lib.a and
  # libocf_cache_lib.a refer to each other (the latter provides the OCF
  # environment adapter), hence the group.
  if(PHOTON_OCF_LIBRARY)
    if(NOT TARGET PHOTON::ocf_cache)
      add_library(PHOTON::ocf_cache STATIC IMPORTED)
      set_target_properties(
        PHOTON::ocf_cache
        PROPERTIES IMPORTED_LOCATION "${PHOTON_OCF_LIBRARY}")
      add_library(PHOTON::ocf STATIC IMPORTED)
      set_target_properties(
        PHOTON::ocf
        PROPERTIES IMPORTED_LOCATION "${PHOTON_OCF_CORE_LIBRARY}")
    endif()
    set(PHOTON_ARCHIVES -Wl,--start-group PHOTON::ocf_cache PHOTON::ocf
        -Wl,--end-group)
  endif()

  list(APPEND PHOTON_ARCHIVES PHOTON::photonlib)

  # Absent when a locally installed photon only ships the merged libphoton.a.
  if(PHOTON_EASY_WEAK_LIBRARY)
    if(NOT TARGET PHOTON::easy_weak)
      add_library(PHOTON::easy_weak STATIC IMPORTED)
      set_target_properties(
        PHOTON::easy_weak
        PROPERTIES IMPORTED_LOCATION "${PHOTON_EASY_WEAK_LIBRARY}")
    endif()
    list(APPEND PHOTON_ARCHIVES PHOTON::easy_weak)
  endif()

  # The weak overrides have to follow the archive referring to them on the link
  # line, hence the aggregate rather than a plain list at every call site.
  if(NOT TARGET PHOTON::photon)
    add_library(PHOTON::photon INTERFACE IMPORTED)
    set_target_properties(
      PHOTON::photon
      PROPERTIES INTERFACE_LINK_LIBRARIES "${PHOTON_ARCHIVES}"
                 INTERFACE_INCLUDE_DIRECTORIES "${PHOTON_INCLUDE_DIR}")
  endif()

  # When photon is built here, the archives above only exist once its own
  # targets have run. The dependency is followed through the aggregate, so
  # everything linking PHOTON::photon waits for them.
  if(DEPENDENCY_PHOTON_REPOSITORY)
    add_dependencies(PHOTON::photonlib photon_static)
    if(TARGET PHOTON::easy_weak)
      add_dependencies(PHOTON::easy_weak easy_weak)
    endif()
    if(TARGET PHOTON::ocf_cache)
      add_dependencies(PHOTON::ocf_cache ocf_cache_lib)
      add_dependencies(PHOTON::ocf ocf_lib)
    endif()
  endif()
endif()
