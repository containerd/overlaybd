#include <string>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <malloc.h>
#include <photon/net/socket.h>
#include <photon/fs/localfs.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/photon.h>
#include <photon/io/signal.h>
#include <chrono>
#include "../gzip/gz.h"
#include "../tar/libtar.h"
#include "../../tools/sha256file.h"


class StreamConvertor {
public:
    std::string get_task_id() {
        auto now = std::chrono::system_clock::now();
        uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        return std::to_string(us)+"." + std::to_string(rand() % 1000000);
    }

    void serve(photon::net::ISocketStream *sock) {
        auto start = std::chrono::steady_clock::now();
        LOG_DEBUG("Accepted");
        auto task_id = get_task_id();
        char recv[65536];

        auto streamfile = open_gzstream_file(sock, 0, true, task_id.c_str());
        DEFER(delete streamfile);
        auto turboOCI_stream = new UnTar(streamfile, nullptr, 0, 4096, nullptr, true);
        DEFER(delete turboOCI_stream);
        auto fn_tar_idx = task_id + ".tar.meta";
        auto tar_idx =  lfs->open(fn_tar_idx.c_str(), O_TRUNC | O_CREAT | O_RDWR, 0644);
        DEFER(delete tar_idx);
        auto nitems = turboOCI_stream->dump_tar_headers(tar_idx);
        LOG_INFO("` items get in `",nitems, fn_tar_idx);
        streamfile->save_index();
        auto dst_tar_idx = streamfile->sha256_checksum() + ".tar.meta";
        if (lfs->rename(fn_tar_idx.c_str(), dst_tar_idx.c_str()) != 0) {
            LOG_ERROR("rename metafile (` --> `) failed.", fn_tar_idx, dst_tar_idx);
        }
        LOG_INFO("save tar meta success. `", dst_tar_idx);
        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>( end - start);
        LOG_INFO("task ` finish. time_elapsed: `ms", task_id, elapsed.count());
    }

    int start_uds_server() {

        srand(time(NULL));
        lfs = photon::fs::new_localfs_adaptor(workdir);
        auto uds_handler = [&](photon::net::ISocketStream *s) -> int {
            LOG_INFO("Accept UDS");
            this->serve(s);
            return 0;
        };
        serv = photon::net::new_uds_server(true);
        assert(0 == serv->bind(uds_path));
        assert(0 == serv->listen(100));
        char path[256];
        serv->getsockname(path, 256);
        if (strcmp(path, uds_path) != 0) {
            LOG_ERRNO_RETURN(0, -1, "get socket name error. ['`' != '`'(expected)]",
                path, uds_path);
        }
        LOG_INFO("uds server listening `", path);
        serv->set_handler(uds_handler);
        serv->start_loop(true);
        return 0;
    }

    int stop() {
        serv->terminate();
        delete serv;
        return 0;
    }

    photon::fs::IFileSystem *lfs = nullptr;
    photon::net::ISocketServer *serv = nullptr;

    const char *uds_path= "/var/run/stream_conv.sock";
    const char *workdir = "/tmp";
} *server;


static void stop_by_signal(int signal) {
    LOG_INFO("Got signal ", signal);
    server->stop();
    LOG_INFO("server stopped");
}

int main(int argc, char *argv[]){
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    // prctl(PR_SET_THP_DISABLE, 1);

    set_log_output_level(1);
    photon::init(photon::INIT_EVENT_DEFAULT | photon::INIT_IO_DEFAULT |
                 photon::INIT_EVENT_SIGNAL);
    //...
    photon::block_all_signal();
    photon::sync_signal(SIGTERM, &stop_by_signal);
    photon::sync_signal(SIGINT, &stop_by_signal);
    photon::sync_signal(SIGTSTP, &stop_by_signal);
    // photon::sync_signal(SIGPIPE, &ignore_signal);
    // photon::sync_signal(SIGUSR2, &restart_by_signal);
    DEFER(photon::fini());
    server = new StreamConvertor;
    DEFER(delete server);
    server->start_uds_server();
    return 0;
}
