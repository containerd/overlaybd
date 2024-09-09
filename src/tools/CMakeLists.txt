add_executable(overlaybd-commit overlaybd-commit.cpp)
target_include_directories(overlaybd-commit PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(overlaybd-commit photon_static overlaybd_image_lib)

add_executable(overlaybd-merge overlaybd-merge.cpp)
target_include_directories(overlaybd-merge PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(overlaybd-merge photon_static overlaybd_image_lib)


add_executable(overlaybd-create overlaybd-create.cpp)
target_include_directories(overlaybd-create PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(overlaybd-create photon_static overlaybd_lib)
set_target_properties(overlaybd-create PROPERTIES INSTALL_RPATH "/opt/overlaybd/lib")

add_executable(overlaybd-zfile overlaybd-zfile.cpp)
target_include_directories(overlaybd-zfile PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(overlaybd-zfile photon_static overlaybd_lib)

add_executable(overlaybd-apply overlaybd-apply.cpp)
target_include_directories(overlaybd-apply PUBLIC ${PHOTON_INCLUDE_DIR} ${rapidjson_SOURCE_DIR}/include)
target_link_libraries(overlaybd-apply photon_static overlaybd_lib overlaybd_image_lib checksum_lib)
set_target_properties(overlaybd-apply PROPERTIES INSTALL_RPATH "/opt/overlaybd/lib")

add_executable(turboOCI-apply turboOCI-apply.cpp)
target_include_directories(turboOCI-apply PUBLIC ${PHOTON_INCLUDE_DIR} ${rapidjson_SOURCE_DIR}/include)
target_link_libraries(turboOCI-apply photon_static overlaybd_lib overlaybd_image_lib)
set_target_properties(turboOCI-apply PROPERTIES INSTALL_RPATH "/opt/overlaybd/lib")

add_library(checksum_lib sha256file.cpp)
target_include_directories(checksum_lib PUBLIC ${PHOTON_INCLUDE_DIR})
target_link_libraries(checksum_lib photon_static)

install(TARGETS
    overlaybd-commit
    overlaybd-create
    overlaybd-zfile
    overlaybd-apply
    overlaybd-merge

    turboOCI-apply
    DESTINATION /opt/overlaybd/bin
)
