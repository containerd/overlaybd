FetchContent_Declare(
  rapidjson
  URL https://github.com/Tencent/rapidjson/archive/v1.1.0.tar.gz
  URL_MD5 badd12c511e081fec6c89c43a7027bce
)
FetchContent_GetProperties(rapidjson)
if (NOT rapidjson_POPULATED)
  FetchContent_Populate(rapidjson)
endif()

add_definitions("-DRAPIDJSON_HAS_STDSTRING=1")