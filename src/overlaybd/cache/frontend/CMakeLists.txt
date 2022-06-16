file(GLOB SRC_FRONTEND "*.cpp")

add_library(cache_frontend_lib STATIC ${SRC_FRONTEND})
target_include_directories(cache_frontend_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)