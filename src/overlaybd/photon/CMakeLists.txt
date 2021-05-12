file(GLOB SOURCE_PHOTON "*.cpp") 
file(GLOB SOURCE_SYNCIO "syncio/*.cpp")
set_source_files_properties(switch_context.s PROPERTIES COMPILE_FLAGS "-x assembler-with-cpp")

find_package(aio REQUIRED)
find_package(CURL REQUIRED)

add_library(photon_lib STATIC
    ${SOURCE_PHOTON}
    ${SOURCE_SYNCIO}
    "switch_context.s"
)

target_include_directories(photon_lib PUBLIC
    ${AIO_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)