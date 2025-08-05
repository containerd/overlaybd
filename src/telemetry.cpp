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
#include "telemetry.h"
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/resource/semantic_conventions.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/exporters/jaeger/jaeger_exporter_options.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <photon/common/alog.h>

namespace overlaybd {
namespace telemetry {

TelemetryManager& TelemetryManager::instance() {
    static TelemetryManager instance;
    return instance;
}

void TelemetryManager::initialize(const std::string& service_name,
                                 const std::string& service_version,
                                 const std::string& otlp_endpoint) {
    if (initialized_) {
        return;
    }
    
    try {
        // Create resource with service information
        auto resource = opentelemetry::sdk::resource::Resource::Create({
            {opentelemetry::sdk::resource::SemanticConventions::kServiceName, service_name},
            {opentelemetry::sdk::resource::SemanticConventions::kServiceVersion, service_version}
        });

        // Create OTLP HTTP exporter
        opentelemetry::exporter::otlp::OtlpHttpExporterOptions otlp_options;
        otlp_options.url = otlp_endpoint;
        auto otlp_exporter = opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create(otlp_options);

        // Create batch span processor
        auto processor = opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(
            std::move(otlp_exporter), {});

        // Create tracer provider
        auto provider = opentelemetry::sdk::trace::TracerProviderFactory::Create(
            std::move(processor), resource);

        // Set the global tracer provider
        opentelemetry::trace::Provider::SetTracerProvider(std::move(provider));

        // Get tracer
        tracer_ = opentelemetry::trace::Provider::GetTracerProvider()->GetTracer(
            service_name, service_version);

        initialized_ = true;
        LOG_INFO("OpenTelemetry initialized for service: ` version: ` endpoint: `", 
                 service_name, service_version, otlp_endpoint);
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to initialize OpenTelemetry: `", e.what());
    }
}

void TelemetryManager::shutdown() {
    if (initialized_) {
        tracer_.reset();
        initialized_ = false;
        LOG_INFO("OpenTelemetry shutdown");
    }
}

opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> TelemetryManager::get_tracer() {
    return tracer_;
}

ScopedSpan::ScopedSpan(const std::string& operation_name) {
    auto tracer = TelemetryManager::instance().get_tracer();
    if (tracer) {
        span_ = tracer->StartSpan(operation_name);
    }
}

ScopedSpan::ScopedSpan(const std::string& operation_name, 
                       const std::map<std::string, std::string>& attributes) {
    auto tracer = TelemetryManager::instance().get_tracer();
    if (tracer) {
        opentelemetry::trace::StartSpanOptions options;
        span_ = tracer->StartSpan(operation_name, options);
        
        if (span_) {
            for (const auto& attr : attributes) {
                span_->SetAttribute(attr.first, attr.second);
            }
        }
    }
}

ScopedSpan::~ScopedSpan() {
    if (span_) {
        span_->End();
    }
}

void ScopedSpan::add_attribute(const std::string& key, const std::string& value) {
    if (span_) {
        span_->SetAttribute(key, value);
    }
}

void ScopedSpan::add_attribute(const std::string& key, int64_t value) {
    if (span_) {
        span_->SetAttribute(key, value);
    }
}

void ScopedSpan::set_status(opentelemetry::trace::StatusCode code, 
                           const std::string& description) {
    if (span_) {
        span_->SetStatus(code, description);
    }
}

} // namespace telemetry
} // namespace overlaybd