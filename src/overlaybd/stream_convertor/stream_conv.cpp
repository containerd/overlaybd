#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <malloc.h>
#include <photon/net/socket.h>
#include <photon/net/http/server.h>
#include <photon/fs/localfs.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/alog-audit.h>
#include <photon/common/string_view.h>
#include <photon/photon.h>
#include <photon/io/signal.h>
#include <chrono>
#include <unistd.h>
#include "config.h"
#include "../gzip/gz.h"
#include "../tar/libtar.h"
#include "../../tools/sha256file.h"
#include "photon/common/alog.h"
#include "photon/net/http/server.h"
#include "yaml-cpp/node/node.h"


App::AppConfig gconfig;

using namespace std;
using namespace photon;
using namespace photon::net;

class StreamConvertor {
public:
    StreamConvertor(App::AppConfig &config) {
        workdir = config.globalConfig().workDir();
        serv_addr = config.globalConfig().udsAddr();
    }

    struct Task {
        Task() {
            auto now = std::chrono::system_clock::now();
            uint64_t us =
                std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
            task_id = std::to_string(us) + "." + std::to_string(rand() % 1000000);
        }
        string task_id;
        int status;
        string msg;
    };

    static int get_task_id(Task &t) {

        return 0;
    }
    bool valid_request(http::Request &req, http::Response &resp) {
        auto body_size = req.headers.content_length();
        LOG_DEBUG("body size: `", body_size);
        if (body_size > 0) {
            return true;
        }
        string str = "{\"code\": -1, \"message\": \"invalid body size(0)\"}\n";
        resp.headers.content_length(str.size());
        resp.write((void*)str.data(), str.size());
        return false;
    }

    int gen_meta(http::Request &req, http::Response &resp, std::string_view){

        Task t;
        auto uuid = req.headers.get_value("UUID").to_string();
        resp.set_result(200);
        if (!valid_request(req, resp)) {
            return 0;
        }
        string msg = string("{\"code\": 0, \"message\":") + uuid + "\"\"}\n";
        if (do_task(&req, t) != 0){
            msg = "failed";
        }
        resp.headers.content_length(msg.size());
        resp.write((void*)msg.data(), msg.size());
        return 0;
    }

    int do_task(IStream *sock, Task &t) {

        struct stat st;
        string filename="";
        SCOPE_AUDIT("gen_meta", AU_FILEOP(filename.c_str(), 0,  st.st_size));

        auto start = std::chrono::steady_clock::now();
        LOG_DEBUG("Accepted");
        auto streamfile = open_gzstream_file(sock, 0, true, t.task_id.c_str(), workdir.c_str());
        DEFER(delete streamfile);
        auto turboOCI_stream = new UnTar(streamfile, nullptr, 0, 4096, nullptr, true);
        DEFER(delete turboOCI_stream);
        auto fn_tar_idx = t.task_id + ".tar.meta";
        auto tar_idx = m_fs->open(fn_tar_idx.c_str(), O_TRUNC | O_CREAT | O_RDWR, 0644);
        DEFER(delete tar_idx);
        auto nitems = turboOCI_stream->dump_tar_headers(tar_idx);
        if (nitems < 0) {
            LOG_ERROR_RETURN(0, -1, "invalid buffer received.");
        }
        LOG_INFO("` items get in `", nitems, fn_tar_idx);

        streamfile->fstat(&st);
        auto fn_gz_idx = streamfile->save_index();
        auto dst_tar_idx = streamfile->sha256_checksum() + ".tar.meta";
        if (m_fs->rename(fn_tar_idx.c_str(), dst_tar_idx.c_str()) != 0) {
            LOG_ERROR("rename metafile (` --> `) failed.", fn_tar_idx, dst_tar_idx);
        }
        LOG_INFO("save meta success. {gz_idx: `, tar_meta: `} ", fn_gz_idx, dst_tar_idx);
        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        auto decomp_speed = (double)st.st_size / 1000 / elapsed.count();
        LOG_INFO("task ` finish. {time_elapsed: `ms, decode_speed: `MB/s}",
            t.task_id, elapsed.count(), decomp_speed);
        filename = dst_tar_idx;

        return 0;
    }



    void serve(photon::net::ISocketStream *sock) {

        Task t;
        auto ret = do_task(sock, t);
        if (ret != 0) {
            sock->close();
        }
    }

    int start() {
        srand(time(NULL));
        if (::access(workdir.c_str(), 0)) {
            if (mkdir(workdir.c_str(), 0755) != 0){
                LOG_ERRNO_RETURN(0, -1, "create workdir failed.");
            }
        }

        std::string httpAddr = gconfig.globalConfig().httpAddr();
        m_tcp_serv = new_tcp_socket_server();
        m_tcp_serv->timeout(1000UL*1000);
        if ( gconfig.globalConfig().reusePort() ){
            m_tcp_serv->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
        }
        if (m_tcp_serv->bind(gconfig.globalConfig().httpPort(), IPAddr(httpAddr.c_str())) != 0) {
            LOG_ERRNO_RETURN(0, -1, "Failed to bind to port ",
                                    gconfig.globalConfig().httpPort());
        };
        if (m_tcp_serv->listen() != 0) {
            LOG_ERRNO_RETURN(0, -1, "Failed to listen socket");
        }

        m_fs = photon::fs::new_localfs_adaptor(workdir.c_str());
        auto uds_handler = [&](photon::net::ISocketStream *s) -> int {
            LOG_INFO("Accept UDS");
            this->serve(s);
            return 0;
        };
        m_http_serv = http::new_http_server();
        m_http_serv->add_handler({this, &StreamConvertor::gen_meta}, "/generateMeta");
        m_tcp_serv->set_handler(m_http_serv->get_connection_handler());

        if (not gconfig.globalConfig().udsAddr().empty()){
            m_uds_serv = photon::net::new_uds_server(true);
            LOG_INFO("try to bind: `", serv_addr);
            if (m_uds_serv->bind(serv_addr.c_str())|| m_uds_serv->listen(100)) {
                LOG_ERRNO_RETURN(0, -1, "bind sock addr failed.");
            }
            char path[256]{};
            if ((m_uds_serv->getsockname(path, 256) < 0) ||  (strcmp(path, serv_addr.c_str()) != 0)) {
                LOG_ERRNO_RETURN(0, -1, "get socket name error. ['`' != '`'(expected)]", path,
                                serv_addr);
            }
            LOG_INFO("uds server listening `", path);
            m_uds_serv->set_handler(uds_handler);
            m_uds_serv->start_loop();
        }
        m_tcp_serv->start_loop(true);

        return 0;
    }

    int stop() {
        delete m_fs;
        if (m_uds_serv) {
            m_uds_serv->terminate();
            delete m_uds_serv;
        }
        if (m_tcp_serv) {
            m_tcp_serv->terminate();

            delete m_tcp_serv;
            delete m_http_serv;
        }
        return 0;
    }

    photon::fs::IFileSystem *m_fs = nullptr;
    photon::net::ISocketServer *m_uds_serv = nullptr, *m_tcp_serv = nullptr;
    photon::net::http::HTTPServer *m_http_serv = nullptr;

    std::string serv_addr;
    std::string workdir;
} * server;

static void stop_by_signal(int signal) {
    LOG_INFO("Got signal ", signal);
    server->stop();
    LOG_INFO("server stopped");
}


int set_log_config(){
    set_log_output_level(gconfig.globalConfig().logConfig().level());
    auto config = gconfig.globalConfig().logConfig();
    if (config.mode() == "file") {
        LOG_INFO("redirect log into `, limitSize: `MB, rotateNums: `",
            config.path(), config.limitSizeMB(), config.rotateNum());
        auto log_fn = config.path();
        default_logger.log_output = new_log_output_file(log_fn.c_str(),
            config.limitSizeMB(),
            config.rotateNum()
        );
        if (!default_logger.log_output) {
            default_logger.log_output = log_output_stdout;
        }
    }

    default_audit_logger.log_output = log_output_stdout;
    return 0;
}

int main(int argc, char *argv[]) {
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    // prctl(PR_SET_THP_DISABLE, 1);

    // set_log_output_level(1);
    photon::init(photon::INIT_EVENT_DEFAULT | photon::INIT_IO_DEFAULT | photon::INIT_EVENT_SIGNAL);
    DEFER(photon::fini());
    DEFER(default_logger.log_output = log_output_null;);

    photon::block_all_signal();
    photon::sync_signal(SIGTERM, &stop_by_signal);
    photon::sync_signal(SIGINT, &stop_by_signal);
    photon::sync_signal(SIGTSTP, &stop_by_signal);

    auto cfg_path = argv[1];
    LOG_INFO("parsing config: `", cfg_path);
    auto node = YAML::LoadFile(cfg_path);
    gconfig = App::mergeConfig(gconfig, node);
    if (gconfig.IsNull()) {
        LOG_ERRNO_RETURN(0, -1, "parse config file failed.");
    }
    set_log_config();
    LOG_INFO("start server...");
    // photon::sync_signal(SIGPIPE, &ignore_signal);
    // photon::sync_signal(SIGUSR2, &restart_by_signal);
    server = new StreamConvertor(gconfig);
    DEFER(delete server);
    server->start();
    return 0;
}
