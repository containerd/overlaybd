include(FetchContent)
include(FindPackageHandleStandardArgs)

if(DEPENDENCY_RAPIDJSON_REPOSITORY)
    if (NOT rapidjson_POPULATED)
        # header only, no need to build it
        FetchContent_Populate(
            rapidjson
            GIT_REPOSITORY ${DEPENDENCY_RAPIDJSON_REPOSITORY}
            GIT_TAG ${DEPENDENCY_RAPIDJSON_TAG}
            GIT_SUBMODULES ""
        )
    endif()
    FetchContent_GetProperties(rapidjson)
    set(RAPIDJSON_INCLUDE_DIRS "${rapidjson_SOURCE_DIR}/include")
    set(RapidJSON_FOUND yes)
else()
    find_package(PkgConfig)
    if (PKG_CONFIG_FOUND)
        pkg_check_modules(RAPIDJSON RapidJSON)
    endif()

    if (NOT RAPIDJSON_FOUND)
        find_path(RAPIDJSON_INCLUDE_DIRS rapidjson/document.h)
        find_package_handle_standard_args(RapidJSON DEFAULT_MSG
                                          RAPIDJSON_INCLUDE_DIRS)
    endif()
endif()

add_definitions("-DRAPIDJSON_HAS_STDSTRING=1")
