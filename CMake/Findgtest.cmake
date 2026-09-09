find_path(GTEST_INCLUDE_DIRS gtest/gtest.h)

find_library(GTEST_LIBRARIES gtest)

# gtest_main is only needed by the tests that do not bring their own main()
find_library(GTEST_MAIN_LIBRARIES gtest_main)

find_package_handle_standard_args(gtest DEFAULT_MSG GTEST_LIBRARIES
                                  GTEST_MAIN_LIBRARIES GTEST_INCLUDE_DIRS)

if(gtest_FOUND AND NOT TARGET GTEST::gtest)
    add_library(GTEST::gtest UNKNOWN IMPORTED)
    set_target_properties(
        GTEST::gtest
        PROPERTIES IMPORTED_LOCATION "${GTEST_LIBRARIES}"
                   INTERFACE_INCLUDE_DIRECTORIES "${GTEST_INCLUDE_DIRS}")
endif()

if(gtest_FOUND AND NOT TARGET GTEST::gtest_main)
    add_library(GTEST::gtest_main UNKNOWN IMPORTED)
    set_target_properties(
        GTEST::gtest_main
        PROPERTIES IMPORTED_LOCATION "${GTEST_MAIN_LIBRARIES}"
                   INTERFACE_LINK_LIBRARIES GTEST::gtest)
endif()

mark_as_advanced(GTEST_INCLUDE_DIRS GTEST_LIBRARIES GTEST_MAIN_LIBRARIES)
