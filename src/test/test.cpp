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

TEST(ImageTest, AccelerateURL) {
    auto tcpserver = photon::net::new_tcp_socket_server();
    tcpserver->timeout(1000UL*1000);
    tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    tcpserver->bind(64208, photon::net::IPAddr("127.0.0.1"));
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto server = photon::net::http::new_http_server();
    DEFER(delete server);
    auto test_handle = [&](void*, photon::net::http::Request &req, photon::net::http::Response &resp, std::string_view) -> int {
        LOG_INFO("test handle...");
        return 0;
    };
    server->add_handler({nullptr, &test_handle});
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();


    EXPECT_EQ(check_accelerate_url("https://127.0.0.1:64208"), true);
    EXPECT_EQ(check_accelerate_url("https://localhost:64208/accelerate"), true);
    EXPECT_EQ(check_accelerate_url("https://127.0.0.1:64208/accelerate"), true);

    EXPECT_EQ(check_accelerate_url("aaa"), false);
    EXPECT_EQ(check_accelerate_url("https://localhost:64209/accelerate"), false);
    EXPECT_EQ(check_accelerate_url("https://127.0.0.1:64209/accelerate"), false);

}

int main(int argc, char** argv) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER(photon::fini(););
    ::testing::InitGoogleTest(&argc, argv);
    auto ret = RUN_ALL_TESTS();
    return ret;
}
