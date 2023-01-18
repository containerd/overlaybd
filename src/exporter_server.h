#pragma once

#include "config.h"
#include "exporter_handler.h"
#include "metrics_fs.h"
#include <photon/net/http/server.h>
#include <photon/net/socket.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/timer.h>
#include <photon/thread/workerpool.h>
#include <pthread.h>
#include <sys/prctl.h>

#include <thread>



class OverlayBDMetric {
public:
    MetricMeta download;
    ExposeMetrics::ExposeRender exporter;

    photon::Timer timer;
    uint64_t m_timeout = -1;

    OverlayBDMetric() : timer(-1, {this, &OverlayBDMetric::on_timer}) {
        exporter.add_latency("download", download.latency);
    }

    ~OverlayBDMetric() {
        timer.cancel();
        timer.stop();
    }

    uint64_t on_timer() {
        return m_timeout;
    }

    uint64_t interval(uint64_t x) { return m_timeout = x; }
    uint64_t interval() { return m_timeout; }
    int start() { return timer.reset(0); }
};

struct ExporterServer {
    photon::WorkPool wp;

    photon::net::http::HTTPServer *httpserver = nullptr;
    photon::net::ISocketServer *tcpserver = nullptr;

    bool ready = false;

    ExporterServer(ImageConfigNS::GlobalConfig &config,
                   OverlayBDMetric *metrics)
        : wp(1, photon::INIT_EVENT_EPOLL, 0, 0) {
        wp.call([&] {
            prctl(PR_SET_THP_DISABLE, 1);
            pthread_setname_np(pthread_self(), "overlaybd-exporter");
            char buffer[64];
            snprintf(buffer, 63, "localhost:%d", config.exporterConfig().port());
            metrics->exporter.nodename = buffer;

            tcpserver = photon::net::new_tcp_socket_server();
            tcpserver->bind(config.exporterConfig().port(), photon::net::IPAddr("127.0.0.1"));

            tcpserver->listen();
            httpserver = photon::net::http::new_http_server();
            httpserver->add_handler(&metrics->exporter);
            tcpserver->set_handler(httpserver->get_connection_handler());
            tcpserver->start_loop();
            ready = true;
        });
    }

    ~ExporterServer() {
        wp.call([&] {
            delete tcpserver;
            delete httpserver;
        });
    }
};