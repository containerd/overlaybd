file(GLOB SOURCE_LSMT "*.cpp")

add_library(lsmt_lib STATIC ${SOURCE_LSMT})
target_include_directories(lsmt_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
