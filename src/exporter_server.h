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

#include <photon/common/metric-meter/metrics.h>
#include <photon/net/http/server.h>
#include <photon/net/socket.h>
#include <photon/thread/thread.h>
#include <photon/thread/timer.h>

#include "config.h"
#include "exporter_handler.h"
#include "metrics_fs.h"

class OverlayBDMetric {
public:
    MetricMeta pread, download;

    ExposeMetrics::ExposeRender exporter;

    OverlayBDMetric() {
        exporter.add_throughput("pread", pread.throughput);
        exporter.add_latency("pread", pread.latency);
        exporter.add_qps("pread", pread.qps);
        exporter.add_count("pread", pread.total);
        exporter.add_throughput("download", download.throughput);
        exporter.add_latency("download", download.latency);
        exporter.add_qps("download", download.qps);
        exporter.add_count("download", download.total);
    }
};

struct ExporterServer {
    photon::net::http::HTTPServer *httpserver = nullptr;
    photon::net::ISocketServer *tcpserver = nullptr;

    bool ready = false;

    ExporterServer(ImageConfigNS::GlobalConfig &config,
                   OverlayBDMetric *metrics) {
        tcpserver = photon::net::new_tcp_socket_server();
        tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
        if (tcpserver->bind(config.exporterConfig().port()) < 0)
            LOG_ERRNO_RETURN(0, , "Failed to bind exporter port `",
                             config.exporterConfig().port());
        if (tcpserver->listen() < 0)
            LOG_ERRNO_RETURN(0, , "Failed to listen exporter port `",
                             config.exporterConfig().port());
        httpserver = photon::net::http::new_http_server();
        httpserver->add_handler(&metrics->exporter, false,
                                config.exporterConfig().uriPrefix());
        tcpserver->set_handler(httpserver->get_connection_handler());
        tcpserver->start_loop();
        ready = true;
    }

    ~ExporterServer() {
        delete tcpserver;
        delete httpserver;
    }
};