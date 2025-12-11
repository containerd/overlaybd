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
#include "../version.h"
#include <photon/net/http/client.h>
#include <photon/fs/localfs.h>

#include <unistd.h>
#include <fcntl.h>

#include "../image_service.cpp"
#include "../image_service.h"
#include "../image_file.h"
#include "../tools/comm_func.h"

char *test_ua = nullptr;

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


int ua_check_handler(void*, photon::net::http::Request &req, photon::net::http::Response &resp, std::string_view) {
    auto ua = req.headers["User-Agent"];
    LOG_DEBUG(VALUE(ua));
    EXPECT_EQ(ua, test_ua);
    resp.set_result(200);
    LOG_INFO("expected UA: `", test_ua);
    std::string str = "success";
    resp.headers.content_length(7);
    resp.write((void*)str.data(), str.size());
    return 0;
}


TEST(http_client, user_agent) {
    auto tcpserver = photon::net::new_tcp_socket_server();
    DEFER(delete tcpserver);
    tcpserver->bind(18731);
    tcpserver->listen();
    auto server = photon::net::http::new_http_server();
    DEFER(delete server);
    server->add_handler({nullptr, &ua_check_handler});
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();

    test_ua = "mytestUA";

    std::string target_get = "http://localhost:18731/file";
    auto client = photon::net::http::new_http_client();
    client->set_user_agent(test_ua);
    DEFER(delete client);
    auto op = client->new_operation(photon::net::http::Verb::GET, target_get);
    DEFER(delete op);
    op->req.headers.content_length(0);
    client->call(op);
    EXPECT_EQ(op->status_code, 200);
    std::string buf;
    buf.resize(op->resp.headers.content_length());
    op->resp.read((void*)buf.data(), op->resp.headers.content_length());
    LOG_DEBUG(VALUE(buf));
    EXPECT_EQ(true, buf == "success");
}

class DevIDTest : public ::testing::Test {
public:
    ImageService *imgservice;
    const std::string test_dir = "/tmp/overlaybd";
    const std::string global_config_path = test_dir + "/global_config.json";
    const std::string image_config_path = test_dir + "/image_config.json";
    const std::string global_config_content = R"delimiter({
  "enableAudit": false,
  "logPath": "",
  "p2pConfig": {
    "enable": false,
    "address": "localhost:64210"
  }
})delimiter";
    const std::string image_config_content = R"delimiter({
    "lowers" : [
        {
            "file" : "/opt/overlaybd/baselayers/ext4_64"
        }
    ]
})delimiter";

    virtual void SetUp() override {
        // set_log_output_level(0);
        system(("mkdir -p " + test_dir).c_str());

        system(("echo \'" + global_config_content + "\' > " + global_config_path).c_str());
        LOG_INFO("Global config file: ");
        system(("cat " + global_config_path).c_str());

        system(("echo \'" + image_config_content + "\' > " + image_config_path).c_str());
        LOG_INFO("Image config file: ");
        system(("cat " + image_config_path).c_str());

        imgservice = create_image_service(global_config_path.c_str());
        if(imgservice == nullptr) {
            LOG_ERROR("failed to create image service");
            exit(-1);
        }
    }
    virtual void TearDown() override {
        delete imgservice;
        system(("rm -rf " + test_dir).c_str());
    }
};

TEST_F(DevIDTest, parse_config_with_dev_id) {
    std::string config_path, dev_id;
    parse_config_and_dev_id("path/to/config.v1.json;123", config_path, dev_id);
    EXPECT_EQ(config_path, "path/to/config.v1.json");
    EXPECT_EQ(dev_id, "123");
}

TEST_F(DevIDTest, parse_config_without_dev_id) {
    std::string config_path, dev_id;
    parse_config_and_dev_id("path/to/config.v1.json", config_path, dev_id);
    EXPECT_EQ(config_path, "path/to/config.v1.json");
    EXPECT_EQ(dev_id, "");
}

TEST_F(DevIDTest, registers) {
    ImageFile* imagefile0 = imgservice->create_image_file(image_config_path.c_str(), "");
    ImageFile* imagefile1 = imgservice->create_image_file(image_config_path.c_str(), "111");
    ImageFile* imagefile2 = imgservice->create_image_file(image_config_path.c_str(), "222");
    ImageFile* imagefile3 = imgservice->create_image_file(image_config_path.c_str(), "333");

    EXPECT_NE(imagefile0, nullptr);
    EXPECT_NE(imagefile1, nullptr);
    EXPECT_NE(imagefile2, nullptr);
    EXPECT_NE(imagefile3, nullptr);

    EXPECT_EQ(imgservice->find_image_file(""), nullptr);
    EXPECT_EQ(imgservice->find_image_file("111"), imagefile1);
    EXPECT_EQ(imgservice->find_image_file("222"), imagefile2);
    EXPECT_EQ(imgservice->find_image_file("333"), imagefile3);

    delete imagefile2;

    EXPECT_EQ(imgservice->find_image_file(""), nullptr);
    EXPECT_EQ(imgservice->find_image_file("111"), imagefile1);
    EXPECT_EQ(imgservice->find_image_file("222"), nullptr);
    EXPECT_EQ(imgservice->find_image_file("333"), imagefile3);

    ImageFile* dup = imgservice->create_image_file(image_config_path.c_str(), "111");

    EXPECT_EQ(dup, nullptr);
    EXPECT_EQ(imgservice->find_image_file("111"), imagefile1);

    delete imagefile0;
    delete imagefile1;
    delete imagefile3;
}

int main(int argc, char** argv) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER(photon::fini(););
    ::testing::InitGoogleTest(&argc, argv);
    auto ret = RUN_ALL_TESTS();
    return ret;
}
