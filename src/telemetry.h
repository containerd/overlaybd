/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#pragma once

#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/jaeger/jaeger_exporter_factory.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/tracer.h>
#include <opentelemetry/common/attribute_value.h>
#include <memory>

namespace overlaybd {
namespace telemetry {

class TelemetryManager {
public:
    static TelemetryManager& instance();
    
    void initialize(const std::string& service_name = "overlaybd",
                   const std::string& service_version = "1.0.0",
                   const std::string& otlp_endpoint = "http://localhost:4318/v1/traces");
    
    void shutdown();
    
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> get_tracer();
    
private:
    TelemetryManager() = default;
    ~TelemetryManager() = default;
    TelemetryManager(const TelemetryManager&) = delete;
    TelemetryManager& operator=(const TelemetryManager&) = delete;
    
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer_;
    bool initialized_ = false;
};

class ScopedSpan {
public:
    ScopedSpan(const std::string& operation_name);
    ScopedSpan(const std::string& operation_name, 
               const std::map<std::string, std::string>& attributes);
    ~ScopedSpan();
    
    void add_attribute(const std::string& key, const std::string& value);
    void add_attribute(const std::string& key, int64_t value);
    void set_status(opentelemetry::trace::StatusCode code, 
                   const std::string& description = "");
    
private:
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span_;
};

#define OTEL_TRACE_SPAN(name) \
    overlaybd::telemetry::ScopedSpan _span(name)

#define OTEL_TRACE_SPAN_WITH_ATTRS(name, attrs) \
    overlaybd::telemetry::ScopedSpan _span(name, attrs)

} // namespace telemetry
} // namespace overlaybd