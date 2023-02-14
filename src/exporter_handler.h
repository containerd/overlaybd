#pragma once

#include "metrics.h"
#include <photon/common/conststr.h>
#include <photon/net/http/server.h>
#include <photon/net/socket.h>
#include "textexporter.h"
#include <zlib.h>


namespace ExposeMetrics {

#define EXPOSE_PHOTON_METRICLIST(name, type)                  \
    std::vector<std::pair<const char*, type*>> va_##name;     \
    void add_##name(const char* tag, type& metric) {          \
        va_##name.emplace_back(std::make_pair(tag, &metric)); \
    }

#define EXPOSE_TEMPLATE(name, ...) static auto name = PROMMETRIC(__VA_ARGS__)

#define LOOP_APPEND_METRIC(ret, name)                                       \
    if (!va_##name.empty()) {                                               \
        ret.append(name.help_str()).append("\n");                           \
        ret.append(name.type_str()).append("\n");                           \
        for (auto x : va_##name) {                                          \
            ret.append(                                                     \
                   name.render(x.second->val(), nodename.c_str(), x.first)) \
                .append("\n");                                              \
        }                                                                   \
        ret.append("\n");                                                   \
    }


struct ExposeRender : public photon::net::http::HTTPHandler {
    EXPOSE_PHOTON_METRICLIST(latency, Metric::AverageLatencyCounter);

    std::string nodename;
    photon::net::http::DelegateHTTPHandler handler;

    template <typename... Args>
    ExposeRender(Args&&... args)
        : nodename(std::forward<Args>(args)...),
          handler{this, &ExposeRender::handle_request} {}

    std::string render() {
        EXPOSE_TEMPLATE(alive, is_alive : gauge{node});
        EXPOSE_TEMPLATE(latency, blob_read_average_latency : gauge{node, type} #us);
        std::string ret(alive.help_str());
        ret.append("\n")
            .append(alive.type_str())
            .append("\n")
            .append(alive.render(1, nodename.c_str()))
            .append("\n\n");
        LOOP_APPEND_METRIC(ret, latency);
        return ret;
    }

    int handle_request(photon::net::http::Request& req, photon::net::http::Response& resp, std::string_view) override {
        std::string str = render();
        auto cl = str.size();
        if (cl > 4096) {
            LOG_ERROR_RETURN(0, -1, "RetType failed test");
        }
        resp.set_result(200);
        resp.headers.content_length(cl);
        resp.write((void*)str.data(), str.size());
        return 0;
    }

    photon::net::http::DelegateHTTPHandler get_handler() { return handler; }
};

#undef LOOP_APPEND_METRIC
#undef EXPOSE_PHOTON_METRICLIST
};  // namespace ExposeMetrics