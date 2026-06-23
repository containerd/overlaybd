include(FetchContent)
set(FETCHCONTENT_QUIET false)
set(PHOTON_ENABLE_EXTFS ON)
set(PHOTON_ENABLE_RESIZE ON)
add_definitions(-DPHOTON_ENABLE_RESIZE)

FetchContent_Declare(
  photon
  GIT_REPOSITORY https://github.com/alibaba/PhotonLibOS.git
  GIT_TAG 0178d14499d8639759a81e32ad58da5226df5e9b
)

if(BUILD_TESTING)
  set(BUILD_TESTING 0)
  FetchContent_MakeAvailable(photon)
  set(BUILD_TESTING 1)
else()
  FetchContent_MakeAvailable(photon)
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
