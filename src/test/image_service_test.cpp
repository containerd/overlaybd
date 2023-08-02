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

#include <gtest/gtest.h>
#include "photon/common/alog.h"
#include "photon/thread/thread.h"
#include "photon/net/http/server.h"
#include "photon/net/socket.h"
#include "photon/photon.h"
#include "photon/net/http/url.h"
#include "photon/net/socket.h"
#include "photon/io/fd-events.h"
#include "photon/net/curl.h"

#include <fcntl.h>

#include "../image_service.cpp"

photon::net::ISocketServer *new_server(std::string ip, uint16_t port) {
    auto server = photon::net::new_tcp_socket_server();
    server->timeout(1000UL*1000);
    server->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    server->bind(port, photon::net::IPAddr(ip.c_str()));
    server->listen();
    server->set_handler(nullptr);
    server->start_loop();
    return server;
}

TEST(ImageTest, AccelerateURL) {
    auto server = new_server("127.0.0.1", 64208);
    DEFER(delete server);

    EXPECT_EQ(check_accelerate_url("https://127.0.0.1:64208"), true);
    EXPECT_EQ(check_accelerate_url("https://localhost:64208/accelerate"), true);
    EXPECT_EQ(check_accelerate_url("https://127.0.0.1:64208/accelerate"), true);

    EXPECT_EQ(check_accelerate_url("aaa"), false);
    EXPECT_EQ(check_accelerate_url("https://localhost:64209/accelerate"), false);
    EXPECT_EQ(check_accelerate_url("https://127.0.0.1:64209/accelerate"), false);

}


int request_metrics() {
      auto request = new photon::net::cURL();
    DEFER({ delete request; });

    auto request_url = "localhost:9863/metrics";
    LOG_INFO("request url: `", request_url);
    photon::net::StringWriter writer;
    auto ret = request->GET(request_url, &writer, (int64_t)1000000);
    if (ret != 200) {
        LOG_ERRNO_RETURN(0, -1, "connect to exporter failed. http response code: `", ret);
    }
    LOG_INFO("response: `", writer.string);
    return 0;
}

TEST(ImageTest, failover) {
    system("mkdir -p /tmp/overlaybd /var/log");
    system("echo \'{\"enableAudit\":false,\"logPath\":\"\",\"p2pConfig\":{\"enable\":true,\"address\":\"localhost:64210\"}}\'>/tmp/overlaybd/config.json");
    ImageService *is = create_image_service("/tmp/overlaybd/config.json");
    is->enable_acceleration();
    EXPECT_EQ(is->global_fs.remote_fs, is->global_fs.cached_fs);
    EXPECT_NE(is->global_fs.remote_fs, is->global_fs.srcfs);

    auto server = new_server("127.0.0.1", 64210);
    is->enable_acceleration();
    EXPECT_NE(is->global_fs.remote_fs, is->global_fs.cached_fs);
    EXPECT_EQ(is->global_fs.remote_fs, is->global_fs.srcfs);

    delete server;
    is->enable_acceleration();
    EXPECT_EQ(is->global_fs.remote_fs, is->global_fs.cached_fs);
    EXPECT_NE(is->global_fs.remote_fs, is->global_fs.srcfs);
    EXPECT_NE(request_metrics(), 0);

    delete is;
}


TEST(ImageTest, enableMetrics) {
    system("mkdir -p /tmp/overlaybd /var/log");
    system("echo \'{\"enableAudit\":false,\"logPath\":\"\",\"p2pConfig\":{\"enable\":true,\"address\":\"localhost:64210\"}, \"exporterConfig\": {\"enable\": true}}\'>/tmp/overlaybd/config.json");
    ImageService *is = create_image_service("/tmp/overlaybd/config.json");
    is->enable_acceleration();
    EXPECT_EQ(is->global_fs.remote_fs, is->global_fs.cached_fs);
    EXPECT_NE(is->global_fs.remote_fs, is->global_fs.srcfs);
    EXPECT_EQ(request_metrics(), 0);

    auto server = new_server("127.0.0.1", 64210);
    is->enable_acceleration();
    EXPECT_NE(is->global_fs.remote_fs, is->global_fs.cached_fs);
    EXPECT_EQ(is->global_fs.remote_fs, is->global_fs.srcfs);
    EXPECT_EQ(request_metrics(), 0);

    delete server;
    delete is;
}

int main(int argc, char** argv) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER(photon::fini(););
    ::testing::InitGoogleTest(&argc, argv);
    auto ret = RUN_ALL_TESTS();
    return ret;
}
