file(GLOB SOURCE_TAR "*.cpp")
add_library(extfs_lib STATIC ${SOURCE_TAR})

if(NOT ORIGIN_EXT2FS)
  find_package(ext2fs REQUIRED)
  add_dependencies(extfs_lib libext2fs)
  target_include_directories(extfs_lib PUBLIC ${EXT2FS_INCLUDE_DIR})
  set(LIB_EXT2FS ${EXT2FS_LIBRARY})

  install(DIRECTORY ${LIBEXT2FS_INSTALL_DIR}/lib DESTINATION /opt/overlaybd/ USE_SOURCE_PERMISSIONS)
else()
  set(LIB_EXT2FS ext2fs)
endif()

target_include_directories(extfs_lib PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(extfs_lib photon_static ${LIB_EXT2FS})

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
