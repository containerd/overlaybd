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
#include "../overlaybd/lsmt/file.h"

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

class DevIDGetTest : public ::testing::Test {
public:
    virtual void SetUp() override {}
    virtual void TearDown() override {}
};

TEST_F(DevIDGetTest, get_dev_id) {
    std::string config_path, dev_id;
    parse_config_and_dev_id("path/to/config.v1.json;123", config_path, dev_id);
    EXPECT_EQ(config_path, "path/to/config.v1.json");
    EXPECT_EQ(dev_id, "123");

    parse_config_and_dev_id("path/to/config.v1.json", config_path, dev_id);
    EXPECT_EQ(config_path, "path/to/config.v1.json");
    EXPECT_EQ(dev_id, "");
}

class DevIDRegisterTest : public DevIDGetTest {
public:
    ImageService *imgservice;
    const std::string test_dir = "/tmp/overlaybd";
    const std::string global_config_path = test_dir + "/global_config.json";
    const std::string image_config_path = test_dir + "/image_config.json";
    std::string global_config_content = R"delimiter({
    "enableAudit": false,
    "logPath": "",
    "p2pConfig": {
        "enable": false,
        "address": "localhost:64210"
    }
})delimiter";
    std::string image_config_content = R"delimiter({
    "lowers" : [
        {
            "file" : "/opt/overlaybd/baselayers/ext4_64"
        }
    ]
})delimiter";

    virtual void SetUp() override {
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

TEST_F(DevIDRegisterTest, register_dev_id) {
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

class HTTPServerTest : public DevIDRegisterTest {
public:
    virtual void SetUp() override {
        global_config_content = R"delimiter({
    "enableAudit": false,
    "logLevel": 1,
    "logPath": "",
    "p2pConfig": {
        "enable": false,
        "address": "localhost:64210"
    },
    "serviceConfig": {
        "enable": true
    }
})delimiter";

        DevIDRegisterTest::SetUp();
    }
    long request_snapshot(const char* request_url) {
        auto request = new photon::net::cURL();
        DEFER({ delete request; });

        LOG_INFO("request url: `", request_url);
        photon::net::StringWriter writer;
        auto ret = request->POST(request_url, &writer, (int64_t)1000000);
        LOG_INFO("response: `", writer.string);
        return ret;
    }
};

TEST_F(HTTPServerTest, http_server) {
    ImageFile* imgfile = imgservice->create_image_file(image_config_path.c_str(), "123");
    EXPECT_NE(imgfile, nullptr);

    EXPECT_EQ(request_snapshot("http://localhost:9862/snapshot"), 400);
    EXPECT_EQ(request_snapshot("http://localhost:9862/snapshot?V#RNWQC&*@#"), 400);
    EXPECT_EQ(request_snapshot("http://localhost:9862/snapshot?dev_id=&config=/tmp/overlaybd/config.json"), 400);
    EXPECT_EQ(request_snapshot("http://localhost:9862/snapshot?dev_id=456&config=/tmp/overlaybd/config.json"), 404);
    EXPECT_EQ(request_snapshot("http://localhost:9862/snapshot?dev_id=123&config=/tmp/overlaybd/config.json"), 500);

    delete imgfile;
}

class CreateSnapshotTest : public DevIDRegisterTest {
public:
    const std::string new_image_config_path = test_dir + "/new_image_config.json";
    std::string new_image_config_content = R"delimiter({
    "lowers" : [
        {
            "file" : "/opt/overlaybd/baselayers/ext4_64"
        },
        {
            "file" : "/tmp/overlaybd/data0.lsmt"
        }
    ],
    "upper": {
        "index": "/tmp/overlaybd/index1.lsmt",
        "data": "/tmp/overlaybd/data1.lsmt"
    }
})delimiter";
    virtual void SetUp() override {
        image_config_content = R"delimiter({
    "lowers" : [
        {
            "file" : "/opt/overlaybd/baselayers/ext4_64"
        }
    ],
    "upper": {
        "index": "/tmp/overlaybd/index0.lsmt",
        "data": "/tmp/overlaybd/data0.lsmt"
    }
})delimiter";

        DevIDRegisterTest::SetUp();

        system(("echo \'" + new_image_config_content + "\' > " + new_image_config_path).c_str());
        LOG_INFO("New image config file: ");
        system(("cat " + new_image_config_path).c_str());

        srand(154574045);
    }

    void create_file_rw(char *data_name, char *index_name, bool sparse = false) {
        auto fdata = photon::fs::open_localfile_adaptor(data_name, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
        auto findex = photon::fs::open_localfile_adaptor(index_name, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
        LSMT::LayerInfo args(fdata, findex);
        args.sparse_rw = sparse;
        args.virtual_size = 64 << 20;
        auto file = LSMT::create_file_rw(args, true);
        delete file;
    }
};

TEST_F(CreateSnapshotTest, create_snapshot) {
    // imagefile0->pwrite( buf, 0, 1MB)
    // imagefile0->restack(xxx) //s config.v1.json.new
    // imagefile1->pread(buf1, 0, 1MB) imagefile0->pread(buf0...)
    create_file_rw("/tmp/overlaybd/data0.lsmt", "/tmp/overlaybd/index0.lsmt");
    create_file_rw("/tmp/overlaybd/data1.lsmt", "/tmp/overlaybd/index1.lsmt");
    
    ImageFile* imgfile0 = imgservice->create_image_file(image_config_path.c_str(), "");
    EXPECT_NE(imgfile0, nullptr);

    auto len = 1 << 20;
    ssize_t ret;
    ALIGNED_MEM4K(buf, len);
    ALIGNED_MEM4K(buf0, len);
    ALIGNED_MEM4K(buf1, len);

    for (auto i = 0; i < len; i++) {
        auto j = rand() % 256;
        buf[i] = j;
    }
    ret = imgfile0->pwrite(buf, len, 0);
    EXPECT_EQ(ret, len);

    EXPECT_EQ(imgfile0->create_snapshot(new_image_config_path.c_str()), 0);

    ImageFile* imgfile1 = imgservice->create_image_file(new_image_config_path.c_str(), "");
    EXPECT_NE(imgfile1, nullptr);

    std::cout << "create_snapshot & verify" << std::endl;
    ret = imgfile0->pread(buf0, len, 0);
    EXPECT_EQ(ret, len);
    ret = imgfile1->pread(buf1, len, 0);
    EXPECT_EQ(ret, len);
    for(auto i = 0; i < len; i++) {
        EXPECT_EQ(buf0[i], buf1[i]);
    }

    for (auto i = 0; i < len / 2; i++) {
        auto j = rand() % 256;
        buf[i] = j;
    }
    ret = imgfile0->pwrite(buf, len / 2, len / 4);
    EXPECT_EQ(ret, len / 2);
    ret = imgfile1->pwrite(buf, len / 2, len / 4);
    EXPECT_EQ(ret, len / 2);

    std::cout << "verify file after pwrite" << std::endl;
    ret = imgfile0->pread(buf0, len, 0);
    EXPECT_EQ(ret, len);
    ret = imgfile1->pread(buf1, len, 0);
    EXPECT_EQ(ret, len);
    for(auto i = 0; i < len; i++) {
        EXPECT_EQ(buf0[i], buf1[i]);
    }

    delete imgfile0;
    delete imgfile1;
}

TEST_F(CreateSnapshotTest, create_snapshot_sparse) {
    create_file_rw("/tmp/overlaybd/data0.lsmt", "/tmp/overlaybd/index0.lsmt", true);
    create_file_rw("/tmp/overlaybd/data1.lsmt", "/tmp/overlaybd/index1.lsmt", true);
    
    ImageFile* imgfile0 = imgservice->create_image_file(image_config_path.c_str(), "");
    EXPECT_NE(imgfile0, nullptr);

    auto len = 1 << 20;
    ssize_t ret;
    ALIGNED_MEM4K(buf, len);
    ALIGNED_MEM4K(buf0, len);
    ALIGNED_MEM4K(buf1, len);

    for (auto i = 0; i < len; i++) {
        auto j = rand() % 256;
        buf[i] = j;
    }
    ret = imgfile0->pwrite(buf, len, 0);
    EXPECT_EQ(ret, len);

    EXPECT_EQ(imgfile0->create_snapshot(new_image_config_path.c_str()), 0);

    ImageFile* imgfile1 = imgservice->create_image_file(new_image_config_path.c_str(), "");
    EXPECT_NE(imgfile1, nullptr);

    std::cout << "create_snapshot & verify" << std::endl;
    ret = imgfile0->pread(buf0, len, 0);
    EXPECT_EQ(ret, len);
    ret = imgfile1->pread(buf1, len, 0);
    EXPECT_EQ(ret, len);
    for(auto i = 0; i < len; i++) {
        EXPECT_EQ(buf0[i], buf1[i]);
    }

    for (auto i = 0; i < len / 2; i++) {
        auto j = rand() % 256;
        buf[i] = j;
    }
    ret = imgfile0->pwrite(buf, len / 2, len / 4);
    EXPECT_EQ(ret, len / 2);
    ret = imgfile1->pwrite(buf, len / 2, len / 4);
    EXPECT_EQ(ret, len / 2);

    std::cout << "verify file after pwrite" << std::endl;
    ret = imgfile0->pread(buf0, len, 0);
    EXPECT_EQ(ret, len);
    ret = imgfile1->pread(buf1, len, 0);
    EXPECT_EQ(ret, len);
    for(auto i = 0; i < len; i++) {
        EXPECT_EQ(buf0[i], buf1[i]);
    }

    delete imgfile0;
    delete imgfile1;
}

TEST_F(CreateSnapshotTest, create_snapshot_failed) {
    create_file_rw("/tmp/overlaybd/data0.lsmt", "/tmp/overlaybd/index0.lsmt");
    ImageFile* imgfile = imgservice->create_image_file(image_config_path.c_str(), "");
    EXPECT_NE(imgfile, nullptr);

    std::cout << "set wrong new lower layer in config file" << std::endl;
    new_image_config_content = R"delimiter({
    "lowers" : [
        {
            "file" : "/opt/overlaybd/baselayers/ext4_64"
        },
        {
            "file" : "/tmp/overlaybd/index0.lsmt"
        }
    ],
    "upper": {
        "index": "/tmp/overlaybd/index1.lsmt",
        "data": "/tmp/overlaybd/data1.lsmt"
    }
})delimiter";
    system(("echo \'" + new_image_config_content + "\' > " + new_image_config_path).c_str());
    EXPECT_EQ(imgfile->create_snapshot(new_image_config_path.c_str()), -1);

    std::cout << "set wrong new upper layer in config file" << std::endl;
    new_image_config_content = R"delimiter({
    "lowers" : [
        {
            "file" : "/opt/overlaybd/baselayers/ext4_64"
        },
        {
            "file" : "/tmp/overlaybd/data0.lsmt"
        }
    ],
    "upper": {
        "index": "/tmp/overlaybd/index1.lsmt",
        "data": "/tmp/overlaybd/data0.lsmt"
    }
})delimiter";
    system(("echo \'" + new_image_config_content + "\' > " + new_image_config_path).c_str());
    EXPECT_EQ(imgfile->create_snapshot(new_image_config_path.c_str()), -1);

    delete imgfile;

    std::cout << "create snapshot for imgfile with only RO layers" << std::endl;
    image_config_content = R"delimiter({
    "lowers" : [
        {
            "file" : "/opt/overlaybd/baselayers/ext4_64"
        }
    ]
})delimiter";
    system(("echo \'" + image_config_content + "\' > " + image_config_path).c_str());
    imgfile = imgservice->create_image_file(image_config_path.c_str(), "");
    EXPECT_NE(imgfile, nullptr);
    EXPECT_EQ(imgfile->create_snapshot(new_image_config_path.c_str()), -1);

    delete imgfile;
}

int main(int argc, char** argv) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER(photon::fini(););
    ::testing::InitGoogleTest(&argc, argv);
    auto ret = RUN_ALL_TESTS();
    return ret;
}
