// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "tracer_common.h"

#include "opentelemetry/context/propagation/global_propagator.h"
#include "opentelemetry/context/propagation/text_map_propagator.h"
#include "opentelemetry/exporters/ostream/span_exporter_factory.h"
#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/provider.h"
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_context.h"
#include "opentelemetry/sdk/trace/tracer_context_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/propagation/http_trace_context.h"
#include "opentelemetry/trace/provider.h"

#include <iostream>
#include <vector>

namespace overlaybd {
namespace otel {

GrpcClientCarrier::GrpcClientCarrier(ClientContext *context) : context_(context) {}

opentelemetry::nostd::string_view GrpcClientCarrier::Get(
    opentelemetry::nostd::string_view /* key */) const noexcept
{
  return "";
}

void GrpcClientCarrier::Set(opentelemetry::nostd::string_view key,
                           opentelemetry::nostd::string_view value) noexcept
{
  std::cout << " Client ::: Adding " << key << " " << value << "\n";
  context_->AddMetadata(std::string(key), std::string(value));
}

GrpcServerCarrier::GrpcServerCarrier(ServerContext *context) : context_(context) {}

opentelemetry::nostd::string_view GrpcServerCarrier::Get(
    opentelemetry::nostd::string_view key) const noexcept
{
  auto it = context_->client_metadata().find({key.data(), key.size()});
  if (it != context_->client_metadata().end())
  {
    return opentelemetry::nostd::string_view(it->second.data(), it->second.size());
  }
  return "";
}

void GrpcServerCarrier::Set(opentelemetry::nostd::string_view /* key */,
                           opentelemetry::nostd::string_view /* value */) noexcept
{
  // Not required for server
}

void InitTracer()
{
  auto exporter = opentelemetry::exporter::trace::OStreamSpanExporterFactory::Create();
  auto processor =
      opentelemetry::sdk::trace::SimpleSpanProcessorFactory::Create(std::move(exporter));
  std::vector<std::unique_ptr<opentelemetry::sdk::trace::SpanProcessor>> processors;
  processors.push_back(std::move(processor));
  // Default is an always-on sampler.
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

}  // namespace otel
}  // namespace overlaybd
