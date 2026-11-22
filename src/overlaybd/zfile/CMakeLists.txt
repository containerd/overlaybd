file(GLOB SOURCE_ZFILE "*.cpp")
file(GLOB SOURCE_LZ4 "lz4/*.c" "lz4/*.cpp")

# ---- QAT auto-detection ----
if (ENABLE_QAT)
    find_path(QAT_INCLUDE_DIR NAMES qat/cpa.h
              PATHS /usr/include/qat /usr/local/include/qat)
    find_library(QAT_LIBRARY NAMES qat)
    find_library(USDM_LIBRARY NAMES usdm)
    if (QAT_INCLUDE_DIR AND QAT_LIBRARY AND USDM_LIBRARY)
        message(STATUS "QAT acceleration: ENABLED (include=${QAT_INCLUDE_DIR})")
    else()
        message(WARNING "ENABLE_QAT=ON but QAT headers/libs not found; disabling QAT")
        set(ENABLE_QAT OFF)
    endif()
endif()
if (NOT ENABLE_QAT)
    # When QAT is off, exclude lz4-qat.cpp so we don't need QAT headers at all
    list(REMOVE_ITEM SOURCE_LZ4 "${CMAKE_CURRENT_SOURCE_DIR}/lz4/lz4-qat.cpp")
    message(STATUS "QAT acceleration: DISABLED")
endif()
# --------------------------------

add_library(zfile_lib STATIC ${SOURCE_ZFILE} ${SOURCE_LZ4})
target_link_libraries(zfile_lib photon_static ${LIBZSTD})

if (ENABLE_QAT)
    target_compile_definitions(zfile_lib PUBLIC -DENABLE_QAT)
    target_include_directories(zfile_lib PUBLIC ${QAT_INCLUDE_DIR})
    target_link_libraries(zfile_lib ${QAT_LIBRARY} ${USDM_LIBRARY} -lpci -lpthread)
endif()

if (BUILD_TESTING)
    add_subdirectory(test)
endif ()
