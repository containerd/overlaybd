// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/context/propagation/text_map_carrier.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/trace/tracer.h"

#include <grpcpp/grpcpp.h>

using grpc::ClientContext;
using grpc::ServerContext;

namespace overlaybd {
namespace otel {

class GrpcClientCarrier : public opentelemetry::context::propagation::TextMapCarrier
{
public:
  GrpcClientCarrier(ClientContext *context);
  GrpcClientCarrier() = default;
  
  opentelemetry::nostd::string_view Get(
      opentelemetry::nostd::string_view key) const noexcept override;
  
  void Set(opentelemetry::nostd::string_view key,
           opentelemetry::nostd::string_view value) noexcept override;

private:
  ClientContext *context_ = nullptr;
};

class GrpcServerCarrier : public opentelemetry::context::propagation::TextMapCarrier
{
public:
  GrpcServerCarrier(ServerContext *context);
  GrpcServerCarrier() = default;
  
  opentelemetry::nostd::string_view Get(
      opentelemetry::nostd::string_view key) const noexcept override;
  
  void Set(opentelemetry::nostd::string_view key,
           opentelemetry::nostd::string_view value) noexcept override;

private:
  ServerContext *context_ = nullptr;
};

void InitTracer();
void CleanupTracer();
opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> get_tracer(std::string tracer_name);

}  // namespace otel
}  // namespace overlaybd