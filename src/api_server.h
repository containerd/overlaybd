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

#include <map>
#include <string>
#include <photon/net/http/server.h>
#include <photon/net/socket.h>

class ImageService;

class ApiHandler : public photon::net::http::HTTPHandler {
public:
    ImageService *imgservice;
    std::map<std::string, std::string> params;

    ApiHandler(ImageService *imgservice) : imgservice(imgservice) {}
    int handle_request(photon::net::http::Request& req,
                       photon::net::http::Response& resp,
                       std::string_view) override;
    void parse_params(std::string_view query);
};

struct ApiServer {
    photon::net::ISocketServer* tcpserver = nullptr;
    photon::net::http::HTTPServer* httpserver = nullptr;
    bool ready = false;

    ApiServer(const std::string &addr, ApiHandler* handler);
    ~ApiServer();
};