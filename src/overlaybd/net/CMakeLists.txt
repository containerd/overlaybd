file(GLOB SOURCE_NET "*.cpp") 

find_package(CURL REQUIRED)

add_library(net_lib STATIC ${SOURCE_NET})
target_include_directories(net_lib PUBLIC ${CURL_INCLUDE_DIRS})
target_link_libraries(net_lib PUBLIC base_lib)