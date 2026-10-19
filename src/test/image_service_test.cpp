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
#include <dirent.h>
#include <fstream>
#include <memory>
#include <thread>
#include <photon/thread/thread11.h>

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

namespace {
std::string upper_test_directory() {
    const char *base = getenv("OBD_TEST_TMPDIR");
    std::string path = std::string(base ? base : "/tmp") + "/upper-engine-XXXXXX";
    std::vector<char> name(path.begin(), path.end());
    name.push_back(0);
    auto result = mkdtemp(name.data());
    return result ? result : "";
}

int upper_fd_flags(const std::string &path) {
    auto directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    DEFER(closedir(directory));
    while (auto entry = readdir(directory)) {
        char *end;
        int fd = strtol(entry->d_name, &end, 10);
        if (*end || end == entry->d_name) continue;
        char name[PATH_MAX];
        auto link = std::string("/proc/self/fd/") + entry->d_name;
        auto length = readlink(link.c_str(), name, sizeof(name) - 1);
        if (length < 0) continue;
        name[length] = 0;
        if (path == name) return fcntl(fd, F_GETFL);
    }
    return -1;
}

void check_upper_engine(uint32_t engine, bool sparse) {
    std::thread worker([=] {
        auto directory = upper_test_directory();
        ASSERT_FALSE(directory.empty());
        auto data = directory + "/data";
        auto index = directory + "/index";
        auto service_path = directory + "/service.json";
        auto image_path = directory + "/image.json";
        {
            std::ofstream service(service_path);
            service << "{\"ioEngine\":" << engine
                    << ",\"enableAudit\":false,\"logPath\":\"\",\"logLevel\":2,"
                    << "\"cacheConfig\":{\"cacheType\":\"file\",\"cacheDir\":\""
                    << directory << "/cache\"},\"gzipCacheConfig\":{\"enable\":false}}";
            std::ofstream image(image_path);
            image << "{\"lowers\":[],\"upper\":{\"data\":\"" << data
                  << "\",\"index\":\"" << index << "\"}}";
        }
        ASSERT_EQ(photon::init(photon::INIT_EVENT_EPOLL,
                               photon::INIT_IO_LIBCURL | photon::INIT_IO_LIBAIO), 0);
        DEFER(photon::fini());
        {
            std::unique_ptr<IFile> fdata(open_localfile_adaptor(data.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600));
            std::unique_ptr<IFile> findex(open_localfile_adaptor(index.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600));
            ASSERT_NE(fdata, nullptr);
            ASSERT_NE(findex, nullptr);
            LSMT::LayerInfo info(fdata.get(), findex.get());
            info.virtual_size = 1 << 20;
            info.rw_type = sparse ? LSMT::RWType::Sparse : LSMT::RWType::Hybrid;
            std::unique_ptr<LSMT::IFileRW> layer(LSMT::create_file_rw(info, false));
            ASSERT_NE(layer, nullptr);
        }
        std::unique_ptr<ImageService> service(create_image_service(service_path.c_str()));
        ASSERT_NE(service, nullptr);
        std::unique_ptr<ImageFile> image(service->create_image_file(image_path.c_str(), ""));
        ASSERT_NE(image, nullptr);
        auto data_flags = upper_fd_flags(data);
        auto index_flags = upper_fd_flags(index);
        ASSERT_GE(data_flags, 0);
        ASSERT_GE(index_flags, 0);
        EXPECT_EQ(bool(data_flags & O_DIRECT), engine == ioengine_libaio);
        EXPECT_EQ(index_flags & O_DIRECT, 0);
        std::vector<char> input(4097, 'A'), output(4097, 0);
        iovec iov{input.data() + 1, 512};
        iovec read_iov{output.data() + 1, 512};
        ASSERT_EQ(image->pwritev(&iov, 1, 0), 512);
        ASSERT_EQ(image->preadv(&read_iov, 1, 0), 512);
        EXPECT_EQ(memcmp(input.data() + 1, output.data() + 1, 512), 0);
        iov.iov_len = read_iov.iov_len = 4096;
        ASSERT_EQ(image->pwritev(&iov, 1, 0), 4096);
        ASSERT_EQ(image->preadv(&read_iov, 1, 0), 4096);
        EXPECT_EQ(memcmp(input.data() + 1, output.data() + 1, 4096), 0);
        iovec split[2]{{input.data() + 1, 512}, {input.data() + 513, 3584}};
        ASSERT_EQ(image->pwritev(split, 2, 4096), 4096);
        iovec read_split[2]{{output.data() + 1, 1536}, {output.data() + 1537, 2560}};
        ASSERT_EQ(image->preadv(read_split, 2, 4096), 4096);
        EXPECT_EQ(memcmp(input.data() + 1, output.data() + 1, 4096), 0);
        iov.iov_len = 512;
        ASSERT_EQ(image->pwritev(&iov, 1, (1 << 20) - 512), 512);
        read_iov.iov_len = 512;
        ASSERT_EQ(image->preadv(&read_iov, 1, (1 << 20) - 512), 512);
        EXPECT_EQ(memcmp(input.data() + 1, output.data() + 1, 512), 0);
        std::vector<photon::join_handle *> writers;
        std::atomic<int> failures{0};
        for (int slot = 0; slot < 8; ++slot) {
            auto thread = photon::thread_create11([&, slot] {
                std::vector<char> buffer(513, 'a' + slot);
                iovec part{buffer.data() + 1, 512};
                for (int n = 0; n < 32; ++n)
                    if (image->pwritev(&part, 1, 8192 + slot * 512) != 512) ++failures;
            });
            writers.push_back(photon::thread_enable_join(thread));
        }
        for (auto thread : writers) photon::thread_join(thread);
        ASSERT_EQ(failures.load(), 0);
        read_iov.iov_len = 4096;
        ASSERT_EQ(image->preadv(&read_iov, 1, 8192), 4096);
        for (int slot = 0; slot < 8; ++slot)
            for (int byte = 0; byte < 512; ++byte)
                ASSERT_EQ(output[1 + slot * 512 + byte], 'a' + slot);
        int last_writer = -1;
        writers.clear();
        for (int writer = 0; writer < 2; ++writer) {
            auto thread = photon::thread_create11([&, writer] {
                std::vector<char> buffer(4097, writer ? 'D' : 'C');
                iovec part{buffer.data() + 1, writer ? size_t(512) : size_t(4096)};
                for (int n = 0; n < 32; ++n) {
                    if (image->pwritev(&part, 1, 16384 + (writer ? 512 : 0)) != (ssize_t)part.iov_len)
                        ++failures;
                    last_writer = writer;
                }
            });
            writers.push_back(photon::thread_enable_join(thread));
        }
        for (auto thread : writers) photon::thread_join(thread);
        ASSERT_EQ(failures.load(), 0);
        ASSERT_EQ(image->preadv(&read_iov, 1, 16384), 4096);
        for (int byte = 0; byte < 4096; ++byte)
            ASSERT_EQ(output[byte + 1], last_writer == 1 && byte >= 512 && byte < 1024 ? 'D' : 'C');
        ASSERT_EQ(image->fdatasync(), 0);
        image.reset();
        image.reset(service->create_image_file(image_path.c_str(), ""));
        ASSERT_NE(image, nullptr);
        ASSERT_EQ(image->preadv(&read_iov, 1, 8192), 4096);
        for (int slot = 0; slot < 8; ++slot)
            for (int byte = 0; byte < 512; ++byte)
                ASSERT_EQ(output[1 + slot * 512 + byte], 'a' + slot);
        read_iov.iov_len = 512;
        ASSERT_EQ(image->preadv(&read_iov, 1, (1 << 20) - 512), 512);
        EXPECT_EQ(memcmp(input.data() + 1, output.data() + 1, 512), 0);
    });
    worker.join();
}
}

TEST(UpperIOEngine, HybridPsync) { check_upper_engine(ioengine_psync, false); }
TEST(UpperIOEngine, SparsePsync) { check_upper_engine(ioengine_psync, true); }
TEST(UpperIOEngine, HybridLibaio) { check_upper_engine(ioengine_libaio, false); }
TEST(UpperIOEngine, SparseLibaio) { check_upper_engine(ioengine_libaio, true); }

int main(int argc, char** argv) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER(photon::fini(););
    ::testing::InitGoogleTest(&argc, argv);
    auto ret = RUN_ALL_TESTS();
    return ret;
}
