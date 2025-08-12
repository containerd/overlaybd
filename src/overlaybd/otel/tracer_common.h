// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "opentelemetry/exporters/otlp/otlp_http_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/provider.h"
#include "opentelemetry/sdk/trace/tracer_context.h"
#include "opentelemetry/sdk/trace/tracer_context_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/sdk/trace/batch_span_processor_factory.h"
#include "opentelemetry/sdk/trace/batch_span_processor_options.h"
#include "opentelemetry/trace/propagation/http_trace_context.h"
#include "opentelemetry/trace/provider.h"

#include <cstring>
#include <iostream>
#include <vector>

namespace overlaybd_otel {

struct TracerConfig {
    std::string endpoint = "http://localhost:4318/v1/traces"; // OTLP HTTP endpoint
    bool use_ssl = false;                                     // Whether to use SSL/TLS
    std::string ssl_cert_path = "";             // Path to SSL certificate (if use_ssl is true)
    bool debug = false;                         // Enable debug logging
    std::map<std::string, std::string> headers; // Custom HTTP headers
};

void InitTracer(const TracerConfig &config = TracerConfig());
void CleanupTracer();
opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> get_tracer(std::string tracer_name);

} // namespace overlaybd_otel