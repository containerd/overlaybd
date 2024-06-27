add_custom_command(
    OUTPUT ${EXECUTABLE_OUTPUT_PATH}/ext4_64
    COMMAND tar -zxvf ${CMAKE_CURRENT_LIST_DIR}/ext4_64.tar.gz -C ${EXECUTABLE_OUTPUT_PATH}
    VERBATIM
)

add_custom_command(
    OUTPUT ${EXECUTABLE_OUTPUT_PATH}/ext4_256
    COMMAND tar -zxvf ${CMAKE_CURRENT_LIST_DIR}/ext4_256.tar.gz -C ${EXECUTABLE_OUTPUT_PATH}
    VERBATIM
)

add_custom_command(
    OUTPUT ${EXECUTABLE_OUTPUT_PATH}/.commit
    COMMAND ${CMAKE_COMMAND} -E create_symlink ext4_256 ${EXECUTABLE_OUTPUT_PATH}/.commit
    VERBATIM
)

add_custom_target(baselayer_64 ALL DEPENDS ${EXECUTABLE_OUTPUT_PATH}/ext4_64)
add_custom_target(baselayer_256 ALL DEPENDS ${EXECUTABLE_OUTPUT_PATH}/ext4_256)
add_custom_target(baselayer_commit ALL DEPENDS ${EXECUTABLE_OUTPUT_PATH}/.commit)

install(FILES
    ${EXECUTABLE_OUTPUT_PATH}/ext4_64
    ${EXECUTABLE_OUTPUT_PATH}/ext4_256
    ${EXECUTABLE_OUTPUT_PATH}/.commit
    DESTINATION /opt/overlaybd/baselayers
)
