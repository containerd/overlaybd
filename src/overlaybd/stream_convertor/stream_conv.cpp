#include <fcntl.h>
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
#include <unistd.h>
#include "config.h"
#include "../gzip/gz.h"
#include "../tar/libtar.h"
#include "../../tools/sha256file.h"
#include "photon/common/alog.h"
#include "yaml-cpp/node/node.h"


App::AppConfig gconfig;

class StreamConvertor {
public:

    StreamConvertor(App::AppConfig &config) {
        workdir = config.globalConfig().workDir();
        serv_addr = config.globalConfig().servAddr();
    }

    std::string get_task_id() {
        auto now = std::chrono::system_clock::now();
        uint64_t us =
            std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        return std::to_string(us) + "." + std::to_string(rand() % 1000000);
    }

    void serve(photon::net::ISocketStream *sock) {
        auto start = std::chrono::steady_clock::now();
        LOG_DEBUG("Accepted");
        auto task_id = get_task_id();

        auto streamfile = open_gzstream_file(sock, 0, true, task_id.c_str(), workdir.c_str());
        DEFER(delete streamfile);
        auto turboOCI_stream = new UnTar(streamfile, nullptr, 0, 4096, nullptr, true);
        DEFER(delete turboOCI_stream);
        auto fn_tar_idx = task_id + ".tar.meta";
        auto tar_idx = lfs->open(fn_tar_idx.c_str(), O_TRUNC | O_CREAT | O_RDWR, 0644);
        DEFER(delete tar_idx);
        auto nitems = turboOCI_stream->dump_tar_headers(tar_idx);
        if (nitems < 0) {
            sock->close();
            LOG_ERROR("invalid buffer received.");
            return;
        }
        LOG_INFO("` items get in `", nitems, fn_tar_idx);
        struct stat st;
        streamfile->fstat(&st);
        auto fn_gz_idx = streamfile->save_index();
        auto dst_tar_idx = streamfile->sha256_checksum() + ".tar.meta";
        if (lfs->rename(fn_tar_idx.c_str(), dst_tar_idx.c_str()) != 0) {
            LOG_ERROR("rename metafile (` --> `) failed.", fn_tar_idx, dst_tar_idx);
        }
        LOG_INFO("save meta success. {gz_idx: `, tar_meta: `} ", fn_gz_idx, dst_tar_idx);
        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        auto decomp_speed = (double)st.st_size / 1000 / elapsed.count();
        LOG_INFO("task ` finish. {time_elapsed: `ms, decode_speed: `MB/s}",
            task_id, elapsed.count(), decomp_speed);
    }

    int start() {
        srand(time(NULL));
        if (::access(workdir.c_str(), 0)) {
            if (mkdir(workdir.c_str(), 0755) != 0){
                LOG_ERRNO_RETURN(0, -1, "create workdir failed.");
            }
        }
        lfs = photon::fs::new_localfs_adaptor(workdir.c_str());
        auto uds_handler = [&](photon::net::ISocketStream *s) -> int {
            LOG_INFO("Accept UDS");
            this->serve(s);
            return 0;
        };
        serv = photon::net::new_uds_server(true);
        LOG_INFO("try to bind: `", serv_addr);
        if (serv->bind(serv_addr.c_str())|| serv->listen(100)) {
            LOG_ERRNO_RETURN(0, -1, "bind sock addr failed.");
        }
        char path[256]{};
        if ((serv->getsockname(path, 256) < 0) ||  (strcmp(path, serv_addr.c_str()) != 0)) {
            LOG_ERRNO_RETURN(0, -1, "get socket name error. ['`' != '`'(expected)]", path,
                             serv_addr);
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
