#include <gtest/gtest.h>

#include "photon/common/alog.h"
#include "photon/common/alog-stdstring.h"

#include "photon/net/http/message.h"
#include "photon/thread/thread.h"
#include "photon/net/http/server.h"
#include "photon/net/socket.h"
#include "photon/photon.h"
#include <rapidjson/document.h>

#include "photon/fs/localfs.h"
#include "photon/io/fd-events.h"
#include <fcntl.h>
#include "../config.h"
#include "../image_service.cpp"

#include <iostream>


using namespace photon::net;
using namespace photon::net::http;
using namespace photon::fs;
class SimpleAuthHandler : public HTTPHandler {
public:
    IFile *m_file = nullptr;
    struct stat m_st;
    void FailedResp(Response &resp, int result = 404) {
        resp.set_result(result);
        resp.headers.content_length(0);
        resp.keep_alive(true);
    }

    virtual int handle_request(Request& req,  Response& resp, std::string_view ) override {

        /*
        // for local test
        auto fn = "/opt/overlaybd/cred.json";
        if (m_file == nullptr) {
            m_file = open_localfile_adaptor(fn, O_RDONLY);
            m_file->fstat(&m_st);
        }
        auto buf = new char[m_st.st_size];
        auto ret_r = m_file->pread(buf, m_st.st_size, 0);
        */

        auto msg = std::string(R"delimiter(
{
    "success": true,
    "traceId": "trace_id",
    "data": {
        "auths": {
            "<your registry>": {
                "username": "",
                "password": ""
            }
        }
    }
})delimiter");
        LOG_INFO("response: `", msg.c_str());
        resp.set_result(200);
        resp.headers.content_length(msg.size());
        resp.keep_alive(true);
        photon::thread_sleep(1);
        auto ret_w = resp.write((void*)msg.c_str(), msg.size());
        if (ret_w != (ssize_t)msg.size()) {
            LOG_ERRNO_RETURN(0, -1,
                    "send body failed, target: `, `", req.target(), VALUE(ret_w));
        } else {
            LOG_DEBUG("Send response header success");
        }
        LOG_DEBUG("send body done");
        return 0;
    }
};

TEST(auth, http_server) {
    auto tcpserver = photon::net::new_tcp_socket_server();
    tcpserver->timeout(1000UL*1000);
    tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    tcpserver->bind(19876, IPAddr("127.0.0.1"));
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto server = new_http_server();
    DEFER(delete server);
    SimpleAuthHandler h;
    server->add_handler(&h, false, "/auth");
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();
    photon::thread_sleep(1);
    std::string remote_path = "", user = "", passwd = "";
    auto ret = load_cred_from_http("http://127.0.0.1:19876/auth", remote_path, user, passwd, 1);
    EXPECT_EQ(ret, -1); // expect timeout
    ret = load_cred_from_http("http://127.0.0.1:19876/auth", remote_path, user, passwd, 2);
    EXPECT_EQ(ret, 0);
    photon::thread_sleep(60);
}

int main(int argc, char** arg) {
    photon::init();
    DEFER(photon::fini());
    set_log_output_level(ALOG_DEBUG);
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}
