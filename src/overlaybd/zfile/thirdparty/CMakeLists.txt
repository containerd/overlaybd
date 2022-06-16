add_custom_target(thirdparty_lib)
set(THIRDPARTY_PATH "${CMAKE_SOURCE_DIR}/src/overlaybd/zfile/thirdparty")

if(ENABLE_DSA)
    add_custom_command(TARGET thirdparty_lib
        COMMAND cd ${THIRDPARTY_PATH}/DML && git checkout 5a2956362d7b1c65d8aee753938ae9bf5cf0b7fd
        COMMAND cd ${THIRDPARTY_PATH}/DML && rm -rf build && mkdir build && cd build && cmake -DCMAKE_INSTALL_PREFIX=dmldir ..
        COMMAND cd ${THIRDPARTY_PATH}/DML/build && cmake --build . --target install
        COMMAND cd ${THIRDPARTY_PATH}/DML/build/dmldir && find ./ -name "libdml*.a" | xargs -i cp {} ${LIBRARY_OUTPUT_PATH}/;
        COMMAND cp -r ${THIRDPARTY_PATH}/DML/build/dmldir/include ${LIBRARY_OUTPUT_PATH}/
    )
endif()

if(ENABLE_ISAL)
    add_custom_command(TARGET thirdparty_lib
        COMMAND cd ${THIRDPARTY_PATH}/isa-l && git checkout ad8dce15c6d3f0c7f3d1b486d9c649ed39223b45
        COMMAND cd ${THIRDPARTY_PATH}/isa-l && ./autogen.sh
        COMMAND cd ${THIRDPARTY_PATH}/isa-l && ./configure
        COMMAND cd ${THIRDPARTY_PATH}/isa-l && make
        COMMAND cp -r ${THIRDPARTY_PATH}/isa-l/.libs/libisal.a ${LIBRARY_OUTPUT_PATH}/
        COMMAND mkdir -p ${LIBRARY_OUTPUT_PATH}/include && cp ${THIRDPARTY_PATH}/isa-l/include/crc.h ${LIBRARY_OUTPUT_PATH}/include/
    )
endif()