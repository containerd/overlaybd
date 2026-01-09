find_package(PkgConfig)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(RAPIDJSON RapidJSON)
endif()

if (NOT RAPIDJSON_FOUND)
    if (NOT rapidjson_POPULATED)
      FetchContent_Populate(
        rapidjson
        GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
        GIT_TAG 80b6d1c83402a5785c486603c5611923159d0894
        GIT_SUBMODULES ""
      )
    endif()
    FetchContent_GetProperties(rapidjson)
    set(RAPIDJSON_INCLUDE_DIRS "${rapidjson_SOURCE_DIR}/include")
endif()

add_definitions("-DRAPIDJSON_HAS_STDSTRING=1")
