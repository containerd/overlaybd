#include "photon/common/alog.h"
#include "photon/thread/thread.h"
#include "photon/net/http/server.h"
#include "photon/net/socket.h"
#include "photon/photon.h"
#include <rapidjson/document.h>

#include "photon/fs/localfs.h"
#include "photon/io/fd-events.h"
#include <fcntl.h>

using namespace photon::net;
using namespace photon::fs;

class SimpleAuthHandler : public HTTPHandler {
public:
    IFile *m_file = nullptr;
    struct stat m_st;
    SimpleAuthHandler(){};
    ~SimpleAuthHandler(){};
    HTTPServerHandler GetHandler() override {
        return {this, &SimpleAuthHandler::HandlerImpl};
    }
    void FailedResp(HTTPServerResponse &resp, int result = 404) {
        resp.SetResult(result);
        resp.ContentLength(0);
        resp.KeepAlive(true);
        resp.Done();
    }

    RetType HandlerImpl(photon::net::HTTPServerRequest& req,  photon::net::HTTPServerResponse& resp ) {
        
        auto fn = "/opt/overlaybd/cred.json";
        if (m_file == nullptr) {
            m_file = open_localfile_adaptor(fn, O_RDONLY);
            if (!m_file) {
               if (!m_file) {
                    FailedResp(resp);
                    LOG_ERROR_RETURN(0, RetType::failed, "open file ` failed", fn);
                }
            } else {
                m_file->fstat(&m_st);
            }
        }
       
        auto buf = new char[m_st.st_size];
        auto ret_r = m_file->pread(buf, m_st.st_size, 0);
        if (ret_r != m_st.st_size){
             LOG_ERROR_RETURN(0, RetType::failed, "read file ` failed", fn);
        }
        auto msg = std::string(
"{\n\
  \"success\": true,\n\
  \"traceId\": \"trace_id\",\n\
  \"data\": ") + std::string(buf) + 
"}"; 
        LOG_INFO("response: `", msg.c_str());
        resp.ContentLength(msg.size());
        resp.KeepAlive(true);
        auto ret = resp.HeaderDone();
        if (ret != RetType::success) {
            LOG_ERRNO_RETURN(0, RetType::failed, "Send response header failed");
        } else {
            LOG_DEBUG("Send response header success");
        }
        
        auto ret_w = resp.Write((void*)msg.c_str(), msg.size());
        if (ret_w != (ssize_t)m_st.st_size) {
            LOG_ERRNO_RETURN(0, RetType::failed, "send body failed, target: `, `!=`", fn, VALUE(ret_w), VALUE(ret_r));
        }
        LOG_DEBUG("send body done");
        return resp.Done();
    }
};

int go_server() {
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(1000UL*1000);
    tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    tcpserver->bind(19876, IPAddr("127.0.0.1"));
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto server = new_http_server();
    DEFER(delete server);
    auto handler = new_mux_handler();
    DEFER(delete handler);
    auto auth = new SimpleAuthHandler;
    DEFER(delete auth);
    handler->AddHandler("/auth", auth);
    server->SetHTTPHandler(handler->GetHandler());
    tcpserver->set_handler(server->GetConnectionHandler());
    tcpserver->start_loop(true);
    return 0;
}

int main(int argc, char** arg) {
    photon::init();
    DEFER(photon::fini());
    set_log_output_level(ALOG_DEBUG);
    return go_server();
}