file(GLOB SOURCE_ELINK "*.cpp")

add_library(elink_lib STATIC ${SOURCE_ELINK})
target_include_directories(elink_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)
target_link_libraries(elink_lib
    photon_static
    yaml-cpp
)

if(BUILD_TESTING)
   add_subdirectory(test)
endif()
