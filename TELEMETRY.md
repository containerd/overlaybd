# OpenTelemetry Instrumentation for OverlayBD

This document describes the OpenTelemetry instrumentation added to the OverlayBD project for observability and distributed tracing.

## Overview

OverlayBD now includes comprehensive OpenTelemetry instrumentation to provide visibility into:

- **Main Process Operations**: TCMU device lifecycle, command handling
- **I/O Operations**: Read/write operations with metrics on throughput, latency, and errors
- **Tool Execution**: CLI tools like `overlaybd-apply` and `turboOCI-apply`
- **Image File Operations**: Layer management and file operations

## Configuration

### Environment Variables

- `OTEL_EXPORTER_OTLP_ENDPOINT`: OTLP endpoint URL (default: `http://localhost:4318/v1/traces`)
- `OTEL_SERVICE_NAME`: Service name for traces (default: service-specific names)
- `OTEL_SERVICE_VERSION`: Service version (default: `1.0.0`)

### Example Configuration

```bash
export OTEL_EXPORTER_OTLP_ENDPOINT="http://jaeger:4318/v1/traces"
export OTEL_SERVICE_NAME="overlaybd-production"
export OTEL_SERVICE_VERSION="1.2.0"
```

## Instrumented Components

### 1. Main TCMU Service (`overlaybd-tcmu`)

**Spans Created:**
- `overlaybd_main`: Main service lifecycle
- `tcmu_dev_open`: Device opening operations
- `tcmu_dev_close`: Device closing operations
- `scsi_*`: Individual SCSI command operations (read, write, inquiry, etc.)

**Attributes:**
- Device configuration paths
- SCSI command types
- Operation timing and status

### 2. Image File Operations

**Spans Created:**
- `image_file_preadv`: Vector read operations
- `image_file_pwritev`: Vector write operations
- `image_file_open_ro`: Read-only file opening

**Attributes:**
- `offset`: File offset being accessed
- `iovcnt`: Number of I/O vectors
- `bytes_requested`: Total bytes requested
- `bytes_read`/`bytes_written`: Actual bytes transferred
- `error_code`: Error codes on failure
- `file_path`: File paths for open operations

### 3. CLI Tools

**overlaybd-apply:**
- `overlaybd_apply_main`: Complete tool execution

**turboOCI-apply:**
- `turboOCI_apply_main`: Complete tool execution

## Trace Structure

### Typical Trace Flow

```
overlaybd_main
├── tcmu_dev_open
│   └── image_file_open_ro
└── scsi_read
    └── image_file_preadv
```

### I/O Operation Traces

Read/write operations include detailed metrics:

```json
{
  "span_name": "image_file_preadv",
  "attributes": {
    "offset": "1048576",
    "iovcnt": "4",
    "bytes_requested": "16384",
    "bytes_read": "16384"
  },
  "status": "OK"
}
```

## Observability Platforms

### Jaeger Setup

1. Start Jaeger:
```bash
docker run -d --name jaeger \
  -p 16686:16686 \
  -p 4317:4317 \
  -p 4318:4318 \
  jaegertracing/all-in-one:latest
```

2. Configure OverlayBD:
```bash
export OTEL_EXPORTER_OTLP_ENDPOINT="http://localhost:4318/v1/traces"
```

3. Access UI: http://localhost:16686

### Grafana + Tempo Setup

1. Configure Tempo as OTLP receiver
2. Set up Grafana to query Tempo
3. Create dashboards for OverlayBD metrics

## Performance Considerations

- **Low Overhead**: Instrumentation adds minimal performance impact
- **Configurable Sampling**: Use environment variables to control trace sampling
- **Batch Export**: Traces are exported in batches to reduce network overhead
- **Asynchronous**: All trace operations are non-blocking

## Span Naming Convention

Spans follow the pattern: `component_operation`

Examples:
- `overlaybd_main`
- `image_file_preadv`
- `tcmu_dev_open`
- `scsi_read`

## Troubleshooting

### No Traces Appearing

1. Check OTLP endpoint connectivity:
```bash
curl -X POST $OTEL_EXPORTER_OTLP_ENDPOINT -H "Content-Type: application/json" -d '{}'
```

2. Verify environment variables:
```bash
echo $OTEL_EXPORTER_OTLP_ENDPOINT
```

3. Check logs for telemetry initialization messages

### High Trace Volume

Reduce sampling rate or filter specific operations:
```bash
export OTEL_TRACES_SAMPLER=traceidratio
export OTEL_TRACES_SAMPLER_ARG=0.1  # 10% sampling
```

## Building with OpenTelemetry

The build system automatically fetches and links OpenTelemetry C++ SDK:

```bash
cmake -B build
cmake --build build
```

Required dependencies are handled automatically via FetchContent.

## Custom Instrumentation

To add instrumentation to new components:

```cpp
#include "telemetry.h"

void my_function() {
    OTEL_TRACE_SPAN("my_operation");
    
    // Add attributes
    overlaybd::telemetry::ScopedSpan span("detailed_operation", {
        {"key", "value"},
        {"count", "42"}
    });
    
    // Set status on errors
    if (error) {
        span.set_status(opentelemetry::trace::StatusCode::kError, "Operation failed");
    }
}
```