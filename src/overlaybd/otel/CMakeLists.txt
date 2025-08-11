set(SOURCE_OTEL
    tracer_common.cpp
)

add_library(otel_lib STATIC ${SOURCE_OTEL})
set_target_properties(otel_lib PROPERTIES LINKER_LANGUAGE CXX)

include(FetchContent)
FetchContent_Declare(
    opentelemetry-cpp
    GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
    GIT_TAG v1.22.0
)
FetchContent_MakeAvailable(opentelemetry-cpp)

target_include_directories(otel_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(otel_lib PUBLIC opentelemetry-cpp)
target_link_libraries(otel_lib PUBLIC gRPC::grpc++)