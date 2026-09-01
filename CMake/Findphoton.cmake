include(FetchContent)
set(FETCHCONTENT_QUIET false)
set(PHOTON_ENABLE_EXTFS ON CACHE BOOL "Build Photon extfs support" FORCE)
set(PHOTON_BUILD_OCF_CACHE ON CACHE BOOL "Build Photon OCF cache support" FORCE)

if(NOT ORIGIN_EXT2FS)
  set(PHOTON_ENABLE_RESIZE ON CACHE BOOL "Build Photon extfs resize support" FORCE)
  add_definitions(-DPHOTON_ENABLE_RESIZE)
else()
  set(PHOTON_ENABLE_RESIZE OFF CACHE BOOL "Build Photon extfs resize support" FORCE)
endif()

FetchContent_Declare(
  photon
  GIT_REPOSITORY https://github.com/alibaba/PhotonLibOS.git
  GIT_TAG v0.9.6
)

if(BUILD_TESTING)
  set(BUILD_TESTING 0)
  FetchContent_MakeAvailable(photon)
  set(BUILD_TESTING 1)
else()
  FetchContent_MakeAvailable(photon)
endif()

# Photon v0.9.5 exposes the OCF headers as <ocf/...>, while the OCF checkout
# contains them directly in its inc/ directory.  When Photon is consumed via
# FetchContent that include layout is not created, so make it explicit for the
# embedded OCF target.
if(TARGET ocf_lib AND NOT TARGET photon_cache_lib)
  FetchContent_GetProperties(ocf_lib SOURCE_DIR PHOTON_OCF_SOURCE_DIR)
  set(PHOTON_OCF_INCLUDE_DIR ${CMAKE_BINARY_DIR}/photon-ocf-include)
  file(MAKE_DIRECTORY ${PHOTON_OCF_INCLUDE_DIR})
  file(REMOVE ${PHOTON_OCF_INCLUDE_DIR}/ocf)
  file(CREATE_LINK ${PHOTON_OCF_SOURCE_DIR}/inc ${PHOTON_OCF_INCLUDE_DIR}/ocf SYMBOLIC)
  target_include_directories(ocf_lib PUBLIC ${PHOTON_OCF_INCLUDE_DIR})

  # The OCF cache target declares its environment adapter but omits the
  # implementations from its source list when embedded as a dependency.
  target_sources(ocf_cache_lib PRIVATE
    ${photon_SOURCE_DIR}/fs/cache/ocf_cache/photon_bindings/env/ocf_env.cpp
    ${photon_SOURCE_DIR}/fs/cache/ocf_cache/photon_bindings/env/utils_mpool.cpp
  )

  # These archives have circular static dependencies.  Keep the rescan group
  # on the consumer side so that cache users link reliably on GNU ld.
  add_library(photon_cache_lib INTERFACE)
  target_link_libraries(photon_cache_lib INTERFACE
    "-Wl,--start-group"
    "$<TARGET_FILE:photon_static>"
    "$<TARGET_FILE:ocf_cache_lib>"
    "$<TARGET_FILE:ocf_lib>"
    "-Wl,--end-group"
  )
  # Photon users do not all depend on OverlayBD's gzip-cache target.  Export
  # the OCF rescan group from photon_static itself so tools such as
  # overlaybd-commit receive the required OCF environment implementation.
  target_link_libraries(photon_static INTERFACE photon_cache_lib)
endif()

if (BUILD_CURL_FROM_SOURCE)
  find_package(OpenSSL REQUIRED)
  find_package(CURL REQUIRED)
  add_dependencies(photon_obj CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
endif()

if(NOT ORIGIN_EXT2FS)
  add_dependencies(photon_obj libext2fs)
endif()

set(PHOTON_INCLUDE_DIR ${photon_SOURCE_DIR}/include/)
