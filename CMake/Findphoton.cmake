include(FetchContent)
set(FETCHCONTENT_QUIET false)
set(PHOTON_ENABLE_EXTFS ON)

FetchContent_Declare(
  photon
  GIT_REPOSITORY https://github.com/alibaba/PhotonLibOS.git
  GIT_TAG v0.6.17
)

if(BUILD_TESTING)
  set(BUILD_TESTING 0)
  FetchContent_MakeAvailable(photon)
  set(BUILD_TESTING 1)
else()
  FetchContent_MakeAvailable(photon)
endif()

if (BUILD_CURL_FROM_SOURCE)
  find_package(openssl REQUIRED)
  find_package(curl REQUIRED)
  add_dependencies(photon_obj CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
endif()

if(NOT ORIGIN_EXT2FS)
  add_dependencies(photon_obj libext2fs)
endif()

set(PHOTON_INCLUDE_DIR ${photon_SOURCE_DIR}/include/)
