file(GLOB SOURCE_BASE "*.cpp")
add_library(base_lib STATIC ${SOURCE_BASE})
target_compile_definitions(base_lib PUBLIC "-DLOGCURRENT")

add_subdirectory(fs)
add_subdirectory(net)
add_subdirectory(photon)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
