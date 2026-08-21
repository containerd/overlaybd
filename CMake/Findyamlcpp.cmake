include(FetchContent)
include(FindPackageHandleStandardArgs)

if(DEPENDENCY_YAML_CPP_REPOSITORY)
  FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY ${DEPENDENCY_YAML_CPP_REPOSITORY}
    GIT_TAG ${DEPENDENCY_YAML_CPP_TAG}
  )
  FetchContent_MakeAvailable(yaml-cpp)
  set(yamlcpp_FOUND yes)
else()
  find_path(YAMLCPP_INCLUDE_DIRS yaml-cpp/yaml.h)
  find_library(YAMLCPP_LIBRARIES yaml-cpp)

  find_package_handle_standard_args(yamlcpp DEFAULT_MSG YAMLCPP_LIBRARIES
                                    YAMLCPP_INCLUDE_DIRS)

  if(yamlcpp_FOUND AND NOT TARGET yaml-cpp)
    add_library(yaml-cpp UNKNOWN IMPORTED)
    set_target_properties(
      yaml-cpp
      PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${YAMLCPP_INCLUDE_DIRS}"
                 IMPORTED_LOCATION "${YAMLCPP_LIBRARIES}")
  endif()
endif()
