set(SOURCE_OTEL
    tracer_common.cpp
)

include(FetchContent)
FetchContent_Declare(
    opentelemetry-cpp
    GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
    GIT_TAG v1.22.0
    CMAKE_ARGS
        -DBUILD_TESTING=OFF
        -DWITH_EXAMPLES=OFF
        -DWITH_METRICS_PREVIEW=OFF
        -DWITH_LOGS_PREVIEW=OFF
        -DWITH_OTLP=ON
        -DWITH_OTLP_GRPC=ON
        -DWITH_OTLP_HTTP=OFF
)
FetchContent_MakeAvailable(opentelemetry-cpp)

# Get the OpenTelemetry source directory after fetching
FetchContent_GetProperties(opentelemetry-cpp)

add_library(otel_lib STATIC ${SOURCE_OTEL})
set_target_properties(otel_lib PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(otel_lib PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${opentelemetry-cpp_SOURCE_DIR}/api/include
    ${opentelemetry-cpp_SOURCE_DIR}/sdk/include
    ${opentelemetry-cpp_SOURCE_DIR}/exporters/otlp/include
    ${opentelemetry-cpp_SOURCE_DIR}/exporters/otlp/include/opentelemetry/exporters/otlp/otlp_grpc_exporter
)

# Link against specific OpenTelemetry targets
target_link_libraries(otel_lib PUBLIC
    opentelemetry_api
    opentelemetry_common
    opentelemetry_resources
    opentelemetry_trace
    opentelemetry_exporter_otlp_grpc
    opentelemetry_proto
    gRPC::grpc++
)