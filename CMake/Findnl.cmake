find_library(LIBNL_LIB nl-3)
find_library(LIBNL_GENL_LIB nl-genl-3)
set(LIBNL_LIBS
  ${LIBNL_LIB}
  ${LIBNL_GENL_LIB}
)

find_path(LIBNL_INCLUDE_DIR
  NAMES
  netlink/netlink.h
  PATH_SUFFIXES
  libnl3
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(nl DEFAULT_MSG LIBNL_LIBS LIBNL_INCLUDE_DIR)

mark_as_advanced(LIBNL_INCLUDE_DIR LIBNL_LIBS)
