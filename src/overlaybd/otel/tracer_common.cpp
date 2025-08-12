#include "tracer_common.h"

namespace overlaybd_otel {

void InitTracer(const TracerConfig& config)
{
    // Create OTLP HTTP exporter configuration
    opentelemetry::exporter::otlp::OtlpHttpExporterOptions opts;
    opts.url = config.endpoint;
    
    if (config.use_ssl && !config.ssl_cert_path.empty()) {
        opts.ssl_ca_cert_path = config.ssl_cert_path;
    }
    
    // Add any custom headers
    for (const auto& header : config.headers) {
        opts.headers[header.first] = header.second;
    }
    
    if (config.debug) {
        opts.console_debug = true;
    }
    
    // Create OTLP/HTTP exporter using the factory
    auto exporter = opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create(opts);
    
    // Create a batch processor for better performance (instead of simple processor)
    opentelemetry::sdk::trace::BatchSpanProcessorOptions options{};
    auto processor = std::unique_ptr<opentelemetry::sdk::trace::SpanProcessor>(
        new opentelemetry::sdk::trace::BatchSpanProcessor(std::move(exporter), options));
    
    std::vector<std::unique_ptr<opentelemetry::sdk::trace::SpanProcessor>> processors;
    processors.push_back(std::move(processor));
    
    // Default is an always-on sampler
    std::unique_ptr<opentelemetry::sdk::trace::TracerContext> context =
        opentelemetry::sdk::trace::TracerContextFactory::Create(std::move(processors));
    std::shared_ptr<opentelemetry::trace::TracerProvider> provider =
        opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(context));
    
    // Set the global trace provider
    opentelemetry::sdk::trace::Provider::SetTracerProvider(provider);

    // set global propagator
    opentelemetry::context::propagation::GlobalTextMapPropagator::SetGlobalPropagator(
        opentelemetry::nostd::shared_ptr<opentelemetry::context::propagation::TextMapPropagator>(
            new opentelemetry::trace::propagation::HttpTraceContext()));
}

void CleanupTracer()
{
    std::shared_ptr<opentelemetry::trace::TracerProvider> none;
    opentelemetry::sdk::trace::Provider::SetTracerProvider(none);
}

opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> get_tracer(std::string tracer_name)
{
    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    return provider->GetTracer(tracer_name);
}

} // namespace overlaybd_otel
