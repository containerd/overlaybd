add_custom_command(
    OUTPUT ${EXECUTABLE_OUTPUT_PATH}/ext4_64
    COMMAND tar -zxvf ${CMAKE_CURRENT_LIST_DIR}/ext4_64.tar.gz -C ${EXECUTABLE_OUTPUT_PATH}
    VERBATIM
)

add_custom_target(baselayer ALL DEPENDS ${EXECUTABLE_OUTPUT_PATH}/ext4_64)

install(FILES ${EXECUTABLE_OUTPUT_PATH}/ext4_64 DESTINATION /opt/overlaybd/baselayers)