set(SOURCE_OTEL
    tracer_common.cpp
)

include(FetchContent)
FetchContent_Declare(
    opentelemetry-cpp
    GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
    GIT_TAG v1.22.0
)

# Set OpenTelemetry build options before making it available
set(BUILD_TESTING OFF CACHE BOOL "Build tests" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
set(OPENTELEMETRY_INSTALL ON CACHE BOOL "Install OpenTelemetry" FORCE)
set(WITH_OTLP_GRPC OFF CACHE BOOL "Build with OTLP GRPC" FORCE)
set(WITH_OTLP_HTTP ON CACHE BOOL "Build with OTLP HTTP" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build with shared library" FORCE)

FetchContent_MakeAvailable(opentelemetry-cpp)

add_library(otel_lib STATIC ${SOURCE_OTEL})
set_target_properties(otel_lib PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(otel_lib PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${opentelemetry-cpp_SOURCE_DIR}/api/include
    ${opentelemetry-cpp_SOURCE_DIR}/sdk/include
    ${opentelemetry-cpp_SOURCE_DIR}/exporters/otlp/include
    ${opentelemetry-cpp_SOURCE_DIR}/ext/include 
)

# Link against specific OpenTelemetry targets
target_link_libraries(otel_lib PRIVATE
    opentelemetry-cpp::api
    opentelemetry-cpp::sdk
    opentelemetry-cpp::trace
    opentelemetry-cpp::otlp_http_client
    opentelemetry-cpp::http_client_curl
    opentelemetry-cpp::ext
    opentelemetry-cpp::otlp_http_exporter
)
