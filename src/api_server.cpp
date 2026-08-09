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

#include <photon/net/http/server.h>
#include <photon/net/socket.h>
#include <photon/net/http/url.h>
#include <map>
#include <string>
#include <string_view>
#include "image_service.h"
#include "image_file.h"
#include "api_server.h"

class ApiHandler : public photon::net::http::HTTPHandler {
public:
    ImageService *imgservice;

    ApiHandler(ImageService *imgservice) : imgservice(imgservice) {}
    int handle_request(photon::net::http::Request& req,
                       photon::net::http::Response& resp,
                       std::string_view) override {
        if (req.verb() != photon::net::http::Verb::POST) {
            resp.set_result(405);
            std::string msg = R"({"success":false,"message":"Method not allowed"})";
            resp.headers.content_length(msg.size());
            return resp.write((void *)msg.data(), msg.size()) == (ssize_t)msg.size() ? 0 : -1;
        }

        auto target = req.target(); // string view, format: /snapshot?dev_id=${devID}&config=${config}
        std::string_view query("");
        auto pos = target.find('?');
        if (pos != std::string_view::npos) {
            query = target.substr(pos + 1);
        }
        LOG_INFO("Snapshot request received");
        std::map<std::string, std::string> params;
        if (!parse_params(query, params)) {
            resp.set_result(400);
            std::string msg = R"({"success":false,"message":"Malformed or duplicate query parameters"})";
            resp.headers.content_length(msg.size());
            return resp.write((void *)msg.data(), msg.size()) == (ssize_t)msg.size() ? 0 : -1;
        }
        auto dev_id = params["dev_id"];
        auto config_path = params["config"];

        int code;
        std::string msg;

        if (dev_id.empty() || config_path.empty()) {
            code = 400;
            msg = std::string(R"delimiter({
        "success": false,
        "message": "Missing dev_id or config in snapshot request"
})delimiter");
            goto EXIT;
        }

        {
            int snap_ret = imgservice->create_snapshot_for_device(dev_id, config_path.c_str());
            if (snap_ret == -2) {
                code = 404;
                msg = std::string(R"delimiter({
        "success": false,
        "message": "Image file not found"
})delimiter");
                goto EXIT;
            }
            if (snap_ret < 0) {
                code = 500;
                msg = std::string(R"delimiter({
        "success": false,
        "message": "Failed to create snapshot"
})delimiter");
                goto EXIT;
            }
        }

        code = 200;
        msg = std::string(R"delimiter({
        "success": true,
        "message": "Snapshot created successfully"
})delimiter");

EXIT:
        resp.set_result(code);
        resp.headers.content_length(msg.size());
        resp.keep_alive(true);
        auto ret_w = resp.write((void*)msg.c_str(), msg.size());
        if (ret_w != (ssize_t)msg.size()) {
            LOG_ERRNO_RETURN(0, -1, "send body failed, target path /snapshot, `", VALUE(ret_w));
        }
        LOG_DEBUG("send body done");
        return 0;
    }

    // Returns false on malformed input or duplicate keys.
    static bool parse_params(std::string_view query,
                             std::map<std::string, std::string> &params) {
        if (query.empty())
            return true;

        size_t start = 0;
        while (start < query.length()) {
            auto end = query.find('&', start);
            if (end == std::string_view::npos) { // last one
                end = query.length();
            }

            auto param = query.substr(start, end - start);
            auto eq_pos = param.find('=');
            std::string decoded_key;
            std::string decoded_value;
            if (eq_pos != std::string_view::npos) {
                auto key = param.substr(0, eq_pos);
                auto value = param.substr(eq_pos + 1);
                decoded_key = photon::net::http::url_unescape(key);
                decoded_value = photon::net::http::url_unescape(value);
            } else {
                decoded_key = photon::net::http::url_unescape(param);
                decoded_value = "";
            }
            if (decoded_key.empty())
                return false;
            if (params.find(decoded_key) != params.end())
                return false;
            params[decoded_key] = decoded_value;
            start = end + 1;
        }
        return true;
    }
};

struct ApiServer {
    photon::net::ISocketServer* tcpserver = nullptr;
    photon::net::http::HTTPServer* httpserver = nullptr;
    ApiHandler* handler = nullptr;

    ApiServer(ImageService *imgservice) : handler(new ApiHandler(imgservice)) {}

    ~ApiServer() {
        delete handler;
        if(tcpserver) {
            safe_delete(tcpserver);
        }
        if(httpserver) {
            safe_delete(httpserver);
        }
    }

    int init(const std::string &addr) {
        photon::net::http::URL url(addr);
        std::string host = url.host().data(); // the string pointed by data() doesn't end up with '\0'
        auto pos = host.find(":");
        if (pos != host.npos) {
            host.resize(pos);
        }
        tcpserver = photon::net::new_tcp_socket_server();
        tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
        if(tcpserver->bind(url.port(), photon::net::IPAddr(host.c_str())) < 0)
            LOG_ERRNO_RETURN(0, -1, "Failed to bind api server port `", url.port());
        if(tcpserver->listen() < 0)
            LOG_ERRNO_RETURN(0, -1, "Failed to listen api server port `", url.port());
        httpserver = photon::net::http::new_http_server();
        httpserver->add_handler(handler, false, "/snapshot");
        tcpserver->set_handler(httpserver->get_connection_handler());
        tcpserver->start_loop();
        LOG_DEBUG("Api server listening on `:`, path: `", host, url.port(), "/snapshot");
        return 0;
    }
};

int start_api_server(ApiServer *&api_server , ImageService *imgservice, const std::string &addr) {
    api_server = new ApiServer(imgservice);
    return api_server->init(addr);
}

int stop_api_server(ApiServer *api_server) {
    if (api_server == nullptr) return 0;
    safe_delete(api_server);
    return 0;
}
