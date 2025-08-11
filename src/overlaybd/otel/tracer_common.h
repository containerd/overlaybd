// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/context/propagation/global_propagator.h"
#include "opentelemetry/context/propagation/text_map_propagator.h"
#include "opentelemetry/exporters/ostream/span_exporter_factory.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/provider.h"
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_context.h"
#include "opentelemetry/sdk/trace/tracer_context_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/propagation/http_trace_context.h"
#include "opentelemetry/trace/provider.h"

#include <grpcpp/grpcpp.h>
#include <cstring>
#include <iostream>
#include <vector>

using grpc::ClientContext;
using grpc::ServerContext;

namespace {

class GrpcClientCarrier : public opentelemetry::context::propagation::TextMapCarrier {
public:
    GrpcClientCarrier(ClientContext *context) : context_(context) {}
    GrpcClientCarrier() = default;
    virtual opentelemetry::nostd::string_view Get(
        opentelemetry::nostd::string_view key) const noexcept override;

    virtual void Set(opentelemetry::nostd::string_view key,
                    opentelemetry::nostd::string_view value) noexcept override;

    ClientContext *context_ = nullptr;
};

class GrpcServerCarrier : public opentelemetry::context::propagation::TextMapCarrier {
public:
    GrpcServerCarrier(ServerContext *context) : context_(context) {}
    GrpcServerCarrier() = default;
    virtual opentelemetry::nostd::string_view Get(
        opentelemetry::nostd::string_view key) const noexcept override;

    virtual void Set(opentelemetry::nostd::string_view key,
                    opentelemetry::nostd::string_view value) noexcept override;

    ServerContext *context_ = nullptr;
};

void InitTracer();
void CleanupTracer();
opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> get_tracer(std::string tracer_name);

} // namespace