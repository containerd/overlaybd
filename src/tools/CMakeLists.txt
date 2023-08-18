add_executable(overlaybd-commit overlaybd-commit.cpp)
target_include_directories(overlaybd-commit PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(overlaybd-commit photon_static overlaybd_lib)

add_executable(overlaybd-create overlaybd-create.cpp)
target_include_directories(overlaybd-create PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(overlaybd-create photon_static overlaybd_lib)
set_target_properties(overlaybd-create PROPERTIES INSTALL_RPATH "/opt/overlaybd/lib")

add_executable(overlaybd-zfile overlaybd-zfile.cpp)
target_include_directories(overlaybd-zfile PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(overlaybd-zfile photon_static overlaybd_lib)

add_executable(overlaybd-apply overlaybd-apply.cpp comm_func.cpp)
target_include_directories(overlaybd-apply PUBLIC ${PHOTON_INCLUDE_DIR} ${rapidjson_SOURCE_DIR}/include)
target_link_libraries(overlaybd-apply photon_static overlaybd_lib overlaybd_image_lib)
set_target_properties(overlaybd-apply PROPERTIES INSTALL_RPATH "/opt/overlaybd/lib")

add_executable(fastoci-apply fastoci-apply.cpp comm_func.cpp)
target_include_directories(fastoci-apply PUBLIC ${PHOTON_INCLUDE_DIR} ${rapidjson_SOURCE_DIR}/include)
target_link_libraries(fastoci-apply photon_static overlaybd_lib overlaybd_image_lib)
set_target_properties(fastoci-apply PROPERTIES INSTALL_RPATH "/opt/overlaybd/lib")


install(TARGETS
    overlaybd-commit
    overlaybd-create
    overlaybd-zfile
    overlaybd-apply
    fastoci-apply
    DESTINATION /opt/overlaybd/bin
)
