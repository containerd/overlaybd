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
#include <array>
#include <cstdlib>
#include <fstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "../image_service.cpp"
#include "../image_service.h"
#include "../image_file.h"
#include "../tools/comm_func.h"
#include "../overlaybd/lsmt/file.h"

char *test_ua = nullptr;

photon::net::ISocketServer *new_server(std::string ip, uint16_t port) {
    auto server = photon::net::new_tcp_socket_server();
    server->timeout(1000UL*1000);
    server->setsockopt<int>(SOL_SOCKET, SO_REUSEPORT, 1);
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
    DEFER(client->destroy_operation(op));
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

class LazyUpperTest : public ::testing::Test {
protected:
    std::string dir;
    std::string data;
    std::string index;
    ImageConfigNS::ImageConfig config;
    ImageService service;

    bool configure_upper(bool create, const std::string &data_path,
                         const std::string &index_path) {
        return config.ParseJSONStream(
            "{\"upper\":{\"create\":" + std::string(create ? "true" : "false") +
            ",\"data\":\"" + data_path + "\",\"index\":\"" + index_path +
            "\",\"vsize\":1}}");
    }

    void SetUp() override {
        char name[] = "/tmp/overlaybd-lazy-upper-XXXXXX";
        auto created = ::mkdtemp(name);
        ASSERT_NE(created, nullptr);
        dir = created;
        data = dir + "/layer-content.bin";
        index = dir + "/layer-map.idx";
        ASSERT_TRUE(service.global_conf.ParseJSONStream("{}"));
        ASSERT_TRUE(configure_upper(true, data, index));
    }

    void TearDown() override {
        if (dir.empty())
            return;
        ::unlink(data.c_str());
        ::unlink(index.c_str());
        ::rmdir(dir.c_str());
    }
};

TEST_F(LazyUpperTest, create_opens_existing_pair_without_truncating) {
    std::array<char, 4096> written;
    written.fill('x');
    struct iovec write_io = {written.data(), written.size()};

    {
        ImageFile image(config, service, "", "");
        ASSERT_EQ(image.m_status, 1);
        ASSERT_EQ(image.pwritev(&write_io, 1, 0), (ssize_t)written.size());
    }

    {
        ImageFile image(config, service, "", "");
        ASSERT_EQ(image.m_status, 1);
        std::array<char, 4096> read{};
        struct iovec read_io = {read.data(), read.size()};
        ASSERT_EQ(image.preadv(&read_io, 1, 0), (ssize_t)read.size());
        EXPECT_EQ(read, written);
    }

    std::array<char, 4096> read{};
    struct iovec read_io = {read.data(), read.size()};
    ASSERT_TRUE(configure_upper(false, data, index));
    {
        ImageFile image(config, service, "", "");
        ASSERT_EQ(image.m_status, 1);
        ASSERT_EQ(image.preadv(&read_io, 1, 0), (ssize_t)read.size());
    }
    EXPECT_EQ(read, written);
}

TEST_F(LazyUpperTest, waits_for_upper_initialization) {
    int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    ASSERT_GE(dir_fd, 0);
    DEFER(::close(dir_fd));
    ASSERT_EQ(::flock(dir_fd, LOCK_EX | LOCK_NB), 0);
    std::ofstream(data).close();
    std::ofstream(index).close();
    ASSERT_EQ(::access(data.c_str(), F_OK), 0);
    ASSERT_EQ(::access(index.c_str(), F_OK), 0);

    bool started = false;
    int status = 0;
    auto worker = photon::thread_enable_join(photon::thread_create11([&] {
        started = true;
        ImageFile image(config, service, "", "");
        status = image.m_status;
    }));
    while (!started)
        photon::thread_yield();
    photon::thread_usleep(10000);
    EXPECT_EQ(status, 0);

    auto fdata = photon::fs::open_localfile_adaptor(data.c_str(), O_RDWR, 0644);
    auto findex = photon::fs::open_localfile_adaptor(index.c_str(), O_RDWR, 0644);
    EXPECT_NE(fdata, nullptr);
    EXPECT_NE(findex, nullptr);
    if (fdata && findex) {
        LSMT::LayerInfo args(fdata, findex);
        args.virtual_size = 1ULL << 30;
        args.rw_type = LSMT::RWType::Append;
        auto file = LSMT::create_file_rw(args, false);
        EXPECT_NE(file, nullptr);
        delete file;
    }
    delete fdata;
    delete findex;

    ::flock(dir_fd, LOCK_UN);
    photon::thread_join(worker);
    EXPECT_EQ(status, 1);
}

TEST_F(LazyUpperTest, create_rejects_partial_pair) {
    for (int mask = 1; mask <= 2; ++mask) {
        if (mask & 1) {
            std::ofstream(data) << "data marker";
        }
        if (mask & 2) {
            std::ofstream(index) << "index marker";
        }

        ImageFile image(config, service, "", "");
        EXPECT_EQ(image.m_status, -1) << "existing paths: " << mask;

        struct stat st;
        if (mask & 1) {
            ASSERT_EQ(::stat(data.c_str(), &st), 0);
            EXPECT_EQ(st.st_size, 11);
        } else {
            EXPECT_EQ(::access(data.c_str(), F_OK), -1);
        }
        if (mask & 2) {
            ASSERT_EQ(::stat(index.c_str(), &st), 0);
            EXPECT_EQ(st.st_size, 12);
        } else {
            EXPECT_EQ(::access(index.c_str(), F_OK), -1);
        }
        ::unlink(data.c_str());
        ::unlink(index.c_str());
    }
}

TEST_F(LazyUpperTest, open_requires_both_existing_paths) {
    ASSERT_TRUE(configure_upper(false, data, index));
    for (int mask = 0; mask <= 2; ++mask) {
        if (mask & 1) {
            std::ofstream(data) << "data marker";
        }
        if (mask & 2) {
            std::ofstream(index) << "index marker";
        }
        ImageFile image(config, service, "", "");
        EXPECT_EQ(image.m_status, -1) << "existing paths: " << mask;
        if (!(mask & 1))
            EXPECT_EQ(::access(data.c_str(), F_OK), -1);
        if (!(mask & 2))
            EXPECT_EQ(::access(index.c_str(), F_OK), -1);
        ::unlink(data.c_str());
        ::unlink(index.c_str());
    }
}

TEST_F(LazyUpperTest, requires_both_paths_in_config) {
    for (bool create : {false, true}) {
        ASSERT_TRUE(configure_upper(create, data, ""));
        ImageFile missing_index(config, service, "", "");
        EXPECT_EQ(missing_index.m_status, -1);

        ASSERT_TRUE(configure_upper(create, "", index));
        ImageFile missing_data(config, service, "", "");
        EXPECT_EQ(missing_data.m_status, -1);
    }
    EXPECT_EQ(::access(data.c_str(), F_OK), -1);
    EXPECT_EQ(::access(index.c_str(), F_OK), -1);
}

TEST_F(LazyUpperTest, rejects_same_path_for_data_and_index) {
    ASSERT_TRUE(configure_upper(true, data, data));
    ImageFile image(config, service, "", "");
    EXPECT_EQ(image.m_status, -1);
    EXPECT_EQ(::access(data.c_str(), F_OK), -1);
}

TEST_F(LazyUpperTest, rejects_lazy_creation_for_target_backed_upper) {
    ASSERT_TRUE(config.ParseJSONStream(
        "{\"upper\":{\"create\":true,\"data\":\"" + data +
        "\",\"index\":\"" + index + "\",\"target\":\"" + dir +
        "/target\",\"vsize\":1}}"));
    ImageFile image(config, service, "", "");
    EXPECT_EQ(image.m_status, -1);
    EXPECT_EQ(::access(data.c_str(), F_OK), -1);
    EXPECT_EQ(::access(index.c_str(), F_OK), -1);
}

TEST_F(LazyUpperTest, requires_vsize_for_upper_only_creation) {
    ASSERT_TRUE(config.ParseJSONStream(
        "{\"upper\":{\"create\":true,\"data\":\"" + data +
        "\",\"index\":\"" + index + "\"}}"));
    ImageFile image(config, service, "", "");
    EXPECT_EQ(image.m_status, -1);
    EXPECT_EQ(::access(data.c_str(), F_OK), -1);
    EXPECT_EQ(::access(index.c_str(), F_OK), -1);
}

TEST_F(LazyUpperTest, rejects_image_without_layers) {
    for (const char* json : {"{}", "{\"upper\":{}}"}) {
        ASSERT_TRUE(config.ParseJSONStream(json));
        ImageFile image(config, service, "", "");
        EXPECT_EQ(image.m_status, -1) << json;
    }
}

TEST_F(LazyUpperTest, rejects_overflowing_upper_vsize) {
    ASSERT_TRUE(config.ParseJSONStream(
        "{\"upper\":{\"create\":true,\"data\":\"" + data +
        "\",\"index\":\"" + index + "\",\"vsize\":17179869184}}"));
    ImageFile image(config, service, "", "");
    EXPECT_EQ(image.m_status, -1);
    EXPECT_EQ(::access(data.c_str(), F_OK), -1);
    EXPECT_EQ(::access(index.c_str(), F_OK), -1);
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

TEST_F(DevIDRegisterTest, empty_upper_remains_read_only) {
    for (const char* upper : {"{}", R"({"index":"","data":"","create":false})"}) {
        {
            std::ofstream out(image_config_path);
            out << R"({"lowers":[{"file":"/opt/overlaybd/baselayers/ext4_64"}],"upper":)"
                << upper << '}';
            ASSERT_TRUE(out.good());
        }

        ImageFile *image = imgservice->create_image_file(image_config_path.c_str(), "");
        ASSERT_NE(image, nullptr);
        EXPECT_TRUE(image->read_only);
        delete image;
    }
}

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
    int request_snapshot(const char* request_url) {
        // auto request = new photon::net::cURL();
        // DEFER({ delete request; });

        // LOG_INFO("request url: `", request_url);
        // photon::net::StringWriter writer;
        // auto ret = request->POST(request_url, &writer, (int64_t)1000000);
        // LOG_INFO("response: `", writer.string);
        // return ret;

        auto client = photon::net::http::new_http_client();
        DEFER(delete client);
        auto op = client->new_operation(photon::net::http::Verb::GET, request_url);
        DEFER(client->destroy_operation(op));
        op->req.headers.content_length(0);
        // std::cout << "op->req.target(): " << op->req.target() << " op->req.query(): " << op->req.query() << std::endl;
        client->call(op);
        auto code = op->status_code;
        std::string buf;
        buf.resize(op->resp.headers.content_length());
        op->resp.read((void*)buf.data(), op->resp.headers.content_length());
        LOG_INFO(VALUE(buf));

        return code;
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

#define PREADV_SINGLE(file, buf, count, offset) ({ \
    struct iovec iov = { .iov_base = (void *)buf, .iov_len = count }; \
    (file)->preadv(&iov, 1, offset); \
})

#define PWRITEV_SINGLE(file, buf, count, offset) ({ \
    struct iovec iov = { .iov_base = (void *)buf, .iov_len = count }; \
    (file)->pwritev(&iov, 1, offset); \
})

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
        args.rw_type = sparse ? LSMT::RWType::Sparse : LSMT::RWType::Append;
        args.virtual_size = 64 << 20;
        auto file = LSMT::create_file_rw(args, true);
        delete file;
    }

    bool enable_lazy_upper() {
        ImageConfigNS::ImageConfig snapshot;
        if (!snapshot.ParseJSONStream(new_image_config_content))
            return false;
        auto &allocator = snapshot.GetAllocator();
        snapshot["upper"].AddMember(rapidjson::Value("create", allocator),
                                     rapidjson::Value(true), allocator);
        std::ofstream out(new_image_config_path);
        out << snapshot.DumpString();
        return out.good();
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
    ret = PWRITEV_SINGLE(imgfile0, buf, len, 0);
    EXPECT_EQ(ret, len);

    EXPECT_EQ(imgfile0->create_snapshot(new_image_config_path.c_str()), 0);

    ImageFile* imgfile1 = imgservice->create_image_file(image_config_path.c_str(), "");
    EXPECT_NE(imgfile1, nullptr);

    std::cout << "create_snapshot & verify" << std::endl;
    ret = PREADV_SINGLE(imgfile0, buf0, len, 0);
    EXPECT_EQ(ret, len);
    ret = PREADV_SINGLE(imgfile1, buf1, len, 0);
    EXPECT_EQ(ret, len);
    for(auto i = 0; i < len; i++) {
        EXPECT_EQ(buf0[i], buf1[i]);
        EXPECT_EQ(buf0[i], buf[i]);
    }

    for (auto i = 0; i < len / 2; i++) {
        auto j = rand() % 256;
        buf[i] = j;
    }
    ret = PWRITEV_SINGLE(imgfile0, buf, len / 2, len / 4);
    EXPECT_EQ(ret, len / 2);
    ret = PWRITEV_SINGLE(imgfile1, buf, len / 2, len / 4);
    EXPECT_EQ(ret, len / 2);

    std::cout << "verify file after pwrite" << std::endl;
    ret = PREADV_SINGLE(imgfile0, buf0, len, 0);
    EXPECT_EQ(ret, len);
    ret = PREADV_SINGLE(imgfile1, buf1, len, 0);
    EXPECT_EQ(ret, len);
    for(auto i = 0; i < len; i++) {
        EXPECT_EQ(buf0[i], buf1[i]);
        if(i >= len / 4 && i < len / 2 + len / 4)
            EXPECT_EQ(buf0[i], buf[i - len / 4]);
    }

    delete imgfile0;
    delete imgfile1;
}

TEST_F(CreateSnapshotTest, create_snapshot_creates_upper) {
    create_file_rw("/tmp/overlaybd/data0.lsmt", "/tmp/overlaybd/index0.lsmt");
    ASSERT_TRUE(enable_lazy_upper());

    ImageFile* image = imgservice->create_image_file(image_config_path.c_str(), "");
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->create_snapshot(new_image_config_path.c_str()), 0);
    EXPECT_EQ(::access("/tmp/overlaybd/data1.lsmt", F_OK), 0);
    EXPECT_EQ(::access("/tmp/overlaybd/index1.lsmt", F_OK), 0);

    ImageConfigNS::ImageConfig saved;
    ASSERT_TRUE(saved.ParseJSON(image_config_path));
    EXPECT_TRUE(saved.upper().create());
    struct stat image_stat;
    ASSERT_EQ(image->fstat(&image_stat), 0);
    EXPECT_EQ(image_stat.st_size, 64 << 20);
    ImageFile* reopened = imgservice->create_image_file(image_config_path.c_str(), "");
    EXPECT_NE(reopened, nullptr);
    delete reopened;
    delete image;
}

TEST_F(CreateSnapshotTest, create_snapshot_keeps_existing_pair_on_open_failure) {
    create_file_rw("/tmp/overlaybd/data0.lsmt", "/tmp/overlaybd/index0.lsmt");
    std::ofstream("/tmp/overlaybd/data1.lsmt") << "data marker";
    std::ofstream("/tmp/overlaybd/index1.lsmt") << "index marker";
    ASSERT_TRUE(enable_lazy_upper());

    ImageFile* image = imgservice->create_image_file(image_config_path.c_str(), "");
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->create_snapshot(new_image_config_path.c_str()), -1);
    struct stat st;
    ASSERT_EQ(::stat("/tmp/overlaybd/data1.lsmt", &st), 0);
    EXPECT_EQ(st.st_size, 11);
    ASSERT_EQ(::stat("/tmp/overlaybd/index1.lsmt", &st), 0);
    EXPECT_EQ(st.st_size, 12);
    delete image;
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
    ret = PWRITEV_SINGLE(imgfile0, buf, len, 0);
    EXPECT_EQ(ret, len);

    EXPECT_EQ(imgfile0->create_snapshot(new_image_config_path.c_str()), 0);

    ImageFile* imgfile1 = imgservice->create_image_file(image_config_path.c_str(), "");
    EXPECT_NE(imgfile1, nullptr);

    std::cout << "create_snapshot & verify" << std::endl;
    ret = PREADV_SINGLE(imgfile0, buf0, len, 0);
    EXPECT_EQ(ret, len);
    ret = PREADV_SINGLE(imgfile1, buf1, len, 0);
    EXPECT_EQ(ret, len);
    for(auto i = 0; i < len; i++) {
        EXPECT_EQ(buf0[i], buf1[i]);
        EXPECT_EQ(buf0[i], buf[i]);
    }

    for (auto i = 0; i < len / 2; i++) {
        auto j = rand() % 256;
        buf[i] = j;
    }
    ret = PWRITEV_SINGLE(imgfile0, buf, len / 2, len / 4);
    EXPECT_EQ(ret, len / 2);
    ret = PWRITEV_SINGLE(imgfile1, buf, len / 2, len / 4);
    EXPECT_EQ(ret, len / 2);

    std::cout << "verify file after pwrite" << std::endl;
    ret = PREADV_SINGLE(imgfile0, buf0, len, 0);
    EXPECT_EQ(ret, len);
    ret = PREADV_SINGLE(imgfile1, buf1, len, 0);
    EXPECT_EQ(ret, len);
    for(auto i = 0; i < len; i++) {
        EXPECT_EQ(buf0[i], buf1[i]);
        if(i >= len / 4 && i < len / 2 + len / 4)
            EXPECT_EQ(buf0[i], buf[i - len / 4]);
    }

    delete imgfile0;
    delete imgfile1;
}

TEST_F(CreateSnapshotTest, create_snapshot_failed) {
    create_file_rw("/tmp/overlaybd/data0.lsmt", "/tmp/overlaybd/index0.lsmt");
    ImageFile* imgfile = imgservice->create_image_file(image_config_path.c_str(), "");
    EXPECT_NE(imgfile, nullptr);

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
