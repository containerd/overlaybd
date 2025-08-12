find_package(gRPC CONFIG REQUIRED)
if(NOT gRPC_FOUND)
    message(FATAL_ERROR "gRPC not found")
endif()
