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

#include <photon/net/http/url.h>
#include <string_view>
#include "image_service.h"
#include "image_file.h"
#include "api_server.h"

int ApiHandler::handle_request(photon::net::http::Request& req,
                    photon::net::http::Response& resp,
                    std::string_view) {
    auto target = req.target(); // string view, format: /snapshot?dev_id=${devID}&config=${config}
    std::string_view query("");
    auto pos = target.find('?');
    if (pos != std::string_view::npos) {
        query = target.substr(pos + 1);
    }
    LOG_DEBUG("Snapshot query: `", query); // string view, format: dev_id=${devID}&config=${config}
    parse_params(query);
    auto dev_id = params["dev_id"];
    auto config_path = params["config"];
    LOG_DEBUG("dev_id: `, config: `", dev_id, config_path);
    
    int code;
    std::string msg;
    ImageFile* img_file = nullptr;

    if (dev_id.empty() || config_path.empty()) {
        code = 400;
        msg = std::string(R"delimiter({
    "success": false,
    "message": "Missing dev_id or config in snapshot request"
})delimiter");
        goto EXIT;
    }

    img_file = imgservice->find_image_file(dev_id);
    if (!img_file) {
        code = 404;
        msg = std::string(R"delimiter({
    "success": false,
    "message": "Image file not found"
})delimiter");
        goto EXIT;
    }

    if (img_file->create_snapshot(config_path.c_str()) < 0) {
        code = 500;
        msg = std::string(R"delimiter({
    "success": false,
    "message": "Failed to create snapshot`"
})delimiter");
        goto EXIT;
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
        LOG_ERRNO_RETURN(0, -1, "send body failed, target: `, `", req.target(), VALUE(ret_w));
    }
    LOG_DEBUG("send body done");
    return 0;
}

void ApiHandler::parse_params(std::string_view query) { // format: dev_id=${devID}&config=${config}...
    if (query.empty())
        return;
    
    size_t start = 0;
    while (start < query.length()) {
        auto end = query.find('&', start);
        if (end == std::string_view::npos) { // last one
            end = query.length();
        }
        
        auto param = query.substr(start, end - start);
        auto eq_pos = param.find('=');
        if (eq_pos != std::string_view::npos) {
            auto key = param.substr(0, eq_pos);
            auto value = param.substr(eq_pos + 1);
            
            // url decode
            auto decoded_key = photon::net::http::url_unescape(key);
            auto decoded_value = photon::net::http::url_unescape(value);
            params[decoded_key] = decoded_value;
        } else {
            // key without value
            auto key = photon::net::http::url_unescape(param);
            params[key] = "";
        }
        start = end + 1;
    }
}

ApiServer::ApiServer(const std::string &addr, ApiHandler* handler) {
    photon::net::http::URL url(addr);
    std::string host = url.host().data(); // the string pointed by data() doesn't end up with '\0'
    auto pos = host.find(":");
    if (pos != host.npos) {
        host.resize(pos);
    }
    tcpserver = photon::net::new_tcp_socket_server();
    tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    if(tcpserver->bind(url.port(), photon::net::IPAddr(host.c_str())) < 0)
        LOG_ERRNO_RETURN(0, , "Failed to bind api server port `", url.port());
    if(tcpserver->listen() < 0)
        LOG_ERRNO_RETURN(0, , "Failed to listen api server port `", url.port());
    httpserver = photon::net::http::new_http_server();
    httpserver->add_handler(handler, false, "/snapshot");
    tcpserver->set_handler(httpserver->get_connection_handler());
    tcpserver->start_loop();
    ready = true;
    LOG_DEBUG("Api server listening on `:`, path: `", host, url.port(), "/snapshot");
}

ApiServer::~ApiServer() {
    delete tcpserver;
    delete httpserver;
}