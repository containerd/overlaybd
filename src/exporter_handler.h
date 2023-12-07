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

#include <photon/common/conststr.h>
#include <photon/common/estring.h>
#include <photon/common/metric-meter/metrics.h>
#include <photon/net/http/server.h>

#include "textexporter.h"

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
            ret.append(name.render(x.second->val(), x.first)).append("\n"); \
        }                                                                   \
        ret.append("\n");                                                   \
    }

struct ExposeRender : public photon::net::http::HTTPHandler {
    EXPOSE_PHOTON_METRICLIST(throughput, Metric::QPSCounter);
    EXPOSE_PHOTON_METRICLIST(qps, Metric::QPSCounter);
    EXPOSE_PHOTON_METRICLIST(latency, Metric::MaxLatencyCounter);
    EXPOSE_PHOTON_METRICLIST(count, Metric::AddCounter);
    EXPOSE_PHOTON_METRICLIST(cache, Metric::ValueCounter);

    template <typename... Args>
    ExposeRender(Args&&... args) {}

    std::string render() {
        EXPOSE_TEMPLATE(alive, OverlayBD_Alive : gauge{node});
        EXPOSE_TEMPLATE(throughput, OverlayBD_Read_Throughtput
                        : gauge{node, type, mode} #Bytes / sec);
        EXPOSE_TEMPLATE(qps, OverlayBD_QPS : gauge{node, type, mode});
        EXPOSE_TEMPLATE(latency, OverlayBD_MaxLatency
                        : gauge{node, type, mode} #us);
        EXPOSE_TEMPLATE(count, OverlayBD_Count : gauge{node, type} #Bytes);
        std::string ret(alive.help_str());
        ret.append("\n")
            .append(alive.type_str())
            .append("\n")
            .append(alive.render(1))
            .append("\n\n");
        LOOP_APPEND_METRIC(ret, throughput);
        LOOP_APPEND_METRIC(ret, qps);
        LOOP_APPEND_METRIC(ret, latency);
        LOOP_APPEND_METRIC(ret, count);
        return ret;
    }

    int handle_request(photon::net::http::Request& req,
                       photon::net::http::Response& resp,
                       std::string_view) override {
        auto body = render();
        resp.set_result(200);
        resp.keep_alive(true);
        resp.headers.insert("Content-Type", "text/plain; version=0.0.4");
        resp.headers.content_length(body.length());
        ssize_t len = 0;
        len = resp.write((void*)body.data(), body.length());
        if (len == (ssize_t)body.length()) {
            return 0;
        } else {
            LOG_ERRNO_RETURN(0, -1, "Failed to write exporter response");
        }
    }
};

#undef LOOP_APPEND_METRIC
#undef EXPOSE_PHOTON_METRICLIST
};  // namespace ExposeMetrics