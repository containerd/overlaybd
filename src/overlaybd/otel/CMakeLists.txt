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
        -DOPENTELEMETRY_INSTALL=ON
        -DWITH_METRICS_PREVIEW=OFF
        -DWITH_LOGS_PREVIEW=OFF
        -DWITH_OTLP_GRPC=OFF
        -DWITH_OTLP_HTTP=ON
        -DWITH_CURL=ON
        -DWITH_NLOHMANN_JSON=ON
)
FetchContent_MakeAvailable(opentelemetry-cpp)

# Get the OpenTelemetry source directory after fetching
FetchContent_GetProperties(opentelemetry-cpp)

add_library(otel_lib STATIC ${SOURCE_OTEL})
set_target_properties(otel_lib PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(otel_lib PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link against specific OpenTelemetry targets
target_link_libraries(otel_lib PRIVATE
    opentelemetry-cpp::api
    opentelemetry-cpp::trace
    opentelemetry-cpp::otlp_http_exporter
)