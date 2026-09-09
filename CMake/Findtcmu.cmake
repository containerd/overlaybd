include(FetchContent)
include(FindPackageHandleStandardArgs)
set(FETCHCONTENT_QUIET false)

if(DEPENDENCY_TCMU_REPOSITORY)
  FetchContent_Declare(tcmu
    GIT_REPOSITORY ${DEPENDENCY_TCMU_REPOSITORY}
    GIT_TAG ${DEPENDENCY_TCMU_TAG}
  )

  # tcmu itself links against libnl, which is only available as a shared
  # library on the target systems. Restore the normal suffix list while it
  # configures itself and its own dependencies, then switch back.
  set(_tcmu_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES_SAVE})

  if(BUILD_TESTING)
    set(BUILD_TESTING 0)
    FetchContent_MakeAvailable(tcmu)
    set(BUILD_TESTING 1)
  else()
    FetchContent_MakeAvailable(tcmu)
  endif()
  set(TCMU_INCLUDE_DIR ${tcmu_SOURCE_DIR}/)
  set(tcmu_FOUND yes)

  set(CMAKE_FIND_LIBRARY_SUFFIXES ${_tcmu_suffixes})
else()
  find_path(TCMU_INCLUDE_DIR libtcmu.h)
  find_library(TCMU_LIBRARY tcmu)

  find_package_handle_standard_args(tcmu DEFAULT_MSG TCMU_LIBRARY
                                    TCMU_INCLUDE_DIR)
endif()

if(tcmu_FOUND AND NOT TARGET TCMU::tcmu)
  if(DEPENDENCY_TCMU_REPOSITORY)
    # tcmu_static is defined by tcmu's own build
    add_library(TCMU::tcmu ALIAS tcmu_static)
  else()
    add_library(TCMU::tcmu STATIC IMPORTED)
    set_target_properties(
      TCMU::tcmu
      PROPERTIES IMPORTED_LINK_INTERFACE_LANGUAGES "C"
                 IMPORTED_LOCATION "${TCMU_LIBRARY}"
                 INTERFACE_INCLUDE_DIRECTORIES "${TCMU_INCLUDE_DIR}")
  endif()
endif()
