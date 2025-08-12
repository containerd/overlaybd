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
#set(WITH_METRICS_PREVIEW OFF CACHE BOOL "Build metrics preview" FORCE)
#set(WITH_LOGS_PREVIEW OFF CACHE BOOL "Build logs preview" FORCE)
set(WITH_OTLP_GRPC OFF CACHE BOOL "Build with OTLP GRPC" FORCE)
set(WITH_OTLP_HTTP ON CACHE BOOL "Build with OTLP HTTP" FORCE)
#set(WITH_CURL ON CACHE BOOL "Build with CURL" FORCE)
#set(WITH_NLOHMANN_JSON ON CACHE BOOL "Build with nlohmann_json" FORCE)

FetchContent_MakeAvailable(opentelemetry-cpp)

# Get the OpenTelemetry source directory after fetching
FetchContent_GetProperties(opentelemetry-cpp)

find_package(opentelemetry-cpp CONFIG REQUIRED COMPONENTS api sdk exporters_otlp_http)
add_library(otel_lib STATIC ${SOURCE_OTEL})
set_target_properties(otel_lib PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(otel_lib PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
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