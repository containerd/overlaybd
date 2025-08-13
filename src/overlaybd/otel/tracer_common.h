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


void InitTracer();
void CleanupTracer();
opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> get_tracer(std::string tracer_name);

} // namespace overlaybd_otel