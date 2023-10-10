include(FetchContent)

if(${BUILD_CURL_FROM_SOURCE})
    message("Add and build standalone libopenssl")
    include(FetchContent)

    # make openssl into bundle
    FetchContent_Declare(
        openssl102
        GIT_REPOSITORY https://github.com/openssl/openssl.git
        GIT_TAG OpenSSL_1_0_2-stable
        GIT_PROGRESS 1)

    FetchContent_GetProperties(openssl102)

    if(NOT TARGET openssl102_static_build)
        if(NOT openssl102_POPULATED)
            FetchContent_Populate(openssl102)
        endif()
        add_custom_command(
            OUTPUT ${openssl102_BINARY_DIR}/lib/libssl.a
            WORKING_DIRECTORY ${openssl102_SOURCE_DIR}
            COMMAND
                sh config -fPIC no-unit-test no-shared
                --openssldir="${openssl102_BINARY_DIR}"
                --prefix="${openssl102_BINARY_DIR}" && make depend -j && make
                -j 8 && make install)
        add_custom_target(openssl102_static_build
                          DEPENDS ${openssl102_BINARY_DIR}/lib/libssl.a)
        make_directory(${openssl102_BINARY_DIR}/include)
    endif()

    set(OPENSSL_FOUND yes)
    set(OPENSSL_ROOT_DIR ${openssl102_BINARY_DIR})
    set(OPENSSL_INCLUDE_DIR ${OPENSSL_ROOT_DIR}/include)
    set(OPENSSL_INCLUDE_DIRS ${OPENSSL_INCLUDE_DIR})
    set(OPENSSL_SSL_LIBRARY ${OPENSSL_ROOT_DIR}/lib/libssl.a)
    set(OPENSSL_SSL_LIBRARIES ${OPENSSL_SSL_LIBRARY})
    set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_ROOT_DIR}/lib/libcrypto.a)
    set(OPENSSL_CRYPTO_LIBRARIES ${OPENSSL_CRYPTO_LIBRARY})
    set(OPENSSL_LINK_DIR ${OPENSSL_ROOT_DIR}/lib)
    set(OPENSSL_LINK_DIRS ${OPENSSL_LINK_DIR})

    if(NOT TARGET OpenSSL::SSL)
        add_library(OpenSSL::SSL STATIC IMPORTED)
        add_dependencies(OpenSSL::SSL openssl102_static_build)
        set_target_properties(
            OpenSSL::SSL
            PROPERTIES IMPORTED_LINK_INTERFACE_LANGUAGES "C"
                       IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
                       INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIRS}"
                       INTERFACE_LINK_LIBRARIES "${OPENSSL_SSL_LIBRARY}")
    endif()

    if(NOT TARGET OpenSSL::Crypto)
        add_library(OpenSSL::Crypto STATIC IMPORTED)
        add_dependencies(OpenSSL::Crypto openssl102_static_build)
        set_target_properties(
            OpenSSL::Crypto
            PROPERTIES IMPORTED_LINK_INTERFACE_LANGUAGES "C"
                       IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
                       INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIRS}"
                       INTERFACE_LINK_LIBRARIES "${OPENSSL_CRYPTO_LIBRARY}")
    endif()
else()
    include(${CMAKE_ROOT}/Modules/FindOpenSSL.cmake)
endif()
