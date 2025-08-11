file(GLOB SOURCE_OTEL "*.{cpp,h}")

add_library(otel_lib STATIC)

include(FetchContent)
FetchContent_Declare(
    opentelemetry-cpp
    GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
    GIT_TAG v1.22.0
)
FetchContent_MakeAvailable(opentelemetry-cpp)
target_link_libraries(otel_lib opentelemetry-cpp)