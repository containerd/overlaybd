set(SOURCE_OTEL
    tracer_common.cpp
)

add_library(otel_lib STATIC ${SOURCE_OTEL})
set_target_properties(otel_lib PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(otel_lib PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${opentelemetry-cpp_SOURCE_DIR}/api/include
    ${opentelemetry-cpp_SOURCE_DIR}/sdk/include
    ${opentelemetry-cpp_SOURCE_DIR}/exporters/otlp/include
    ${opentelemetry-cpp_SOURCE_DIR}/ext/include
    ${opentelemetry-cpp_SOURCE_DIR}/exporters/otlp/include/opentelemetry/exporters/otlp/otlp_http_exporter
    ${opentelemetry-cpp_SOURCE_DIR}/sdk/include/opentelemetry/sdk/trace
    ${opentelemetry-cpp_SOURCE_DIR}/exporters/otlp/include/opentelemetry/exporters/otlp
)

# Link against specific OpenTelemetry targets
target_link_libraries(otel_lib PUBLIC
    opentelemetry-cpp::sdk
    opentelemetry-cpp::otlp_http_exporter
    opentelemetry-cpp::http_client_curl
    CURL::libcurl
)