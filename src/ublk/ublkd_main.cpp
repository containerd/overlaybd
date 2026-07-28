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

// overlaybd-ublkd: the centralized ublk device manager daemon (ADR-0006 M1).
// All devices live in this process (one queue thread each) and share one
// ImageService; control plane is HTTP over a unix domain socket
// (docker.sock style), M1 handles requests globally serialized.

#include "ublk_device.h"
#include "ublkd_protocol.h"

#include "../image_service.h"
#include "../version.h"

#include <photon/common/alog.h>
#include <photon/io/signal.h>
#include <photon/net/http/message.h>
#include <photon/net/http/server.h>
#include <photon/net/socket.h>
#include <photon/photon.h>

#include <csignal>
#include <climits>
#include <fcntl.h>
#include <map>
#include <memory>
#include <set>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "../tools/CLI11.hpp"

#define UBLKD_DEFAULT_SOCK OVERLAYBD_UBLK_RUN_DIR "/ublkd.sock"

class UblkdServer : public photon::net::http::HTTPHandler {
public:
    UblkdServer(ImageService *service, std::string cache_base)
        : service_(service), cache_base_(std::move(cache_base)) {
    }

    ~UblkdServer() {
        // stop all devices in parallel: STOP first, join afterwards, so the
        // total teardown time is about the slowest device instead of the sum
        // (the bounded-DEL fallback costs 2s per device on backport kernels)
        for (auto &kv : devices_)
            kv.second.dev->stop();
        for (auto &kv : devices_) {
            kv.second.dev->wait();
            kv.second.dev->teardown();
        }
        devices_.clear();
    }

    int handle_request(photon::net::http::Request &req, photon::net::http::Response &resp,
                       std::string_view) override {
        std::string target(req.target());
        std::string body = read_body(req);
        int code = 404;
        std::string msg = ublkd_msg_error("unknown endpoint: " + target);

        if (target == "/v1/add" && req.verb() == photon::net::http::Verb::POST) {
            handle_add(body, code, msg);
        } else if (target == "/v1/del" && req.verb() == photon::net::http::Verb::POST) {
            handle_del(body, code, msg);
        } else if (target == "/v1/list") {
            handle_list(code, msg);
        } else if (target == "/v1/ping") {
            code = 200;
            msg = ublkd_msg_ping(OVERLAYBD_VERSION);
        } else if (target == "/v1/shutdown" &&
                   req.verb() == photon::net::http::Verb::POST) {
            code = 200;
            msg = ublkd_msg_ok();
            shutdown_requested = true;
        }

        resp.set_result(code);
        resp.headers.content_length(msg.size());
        ssize_t w = resp.write((void *)msg.c_str(), msg.size());
        if (w != (ssize_t)msg.size())
            LOG_ERRNO_RETURN(0, -1, "ublkd: send response failed, target: `", target);
        return 0;
    }

    bool shutdown_requested = false;

private:
    static std::string read_body(photon::net::http::Request &req) {
        std::string body;
        size_t n = req.body_size();
        if (n == 0 || n > 64 * 1024) // requests are tiny; cap for safety
            return body;
        body.resize(n);
        ssize_t r = req.read(&body[0], n);
        if (r != (ssize_t)n)
            body.clear();
        return body;
    }

    void handle_add(const std::string &body, int &code, std::string &msg) {
        UblkdAddRequest areq;
        std::string err;
        if (ublkd_parse_add(body, areq, err) != 0) {
            code = 400;
            msg = ublkd_msg_error(err);
            return;
        }
        // Per-image in-flight guard (M2-2). Note: since M2-1 the same-image
        // RW race is already closed by the inst0 flock (LOCK_EX excludes
        // between fds of one process too); this guard is UX -- reject the
        // duplicate immediately instead of failing it after seconds of work.
        char resolved[PATH_MAX];
        if (realpath(areq.config.c_str(), resolved) == nullptr) {
            code = 400;
            msg = ublkd_msg_error("image config " + areq.config + ": " +
                                  strerror(errno));
            return;
        }
        if (!adds_in_flight_.insert(resolved).second) {
            code = 409;
            msg = ublkd_msg_error("an add of this image is already in progress");
            return;
        }

        auto dev = std::make_unique<UblkDevice>(service_);
        UblkDeviceOpts opts;
        opts.image_config_path = areq.config;
        opts.dev_id = areq.dev_id;
        opts.queue_depth = areq.queue_depth;
        // cache tree is shared daemon-wide; cache_dir here only tells the
        // device where the cross-process RW image locks live (M2-1)
        opts.cache_dir = cache_base_;
        int ret = dev->start(opts);
        adds_in_flight_.erase(resolved);
        if (ret != 0) {
            code = 500;
            msg = ublkd_msg_error("device failed to start (see daemon log)");
            return;
        }
        int id = dev->dev_id();
        auto &entry = devices_[id];
        entry.dev = std::move(dev);
        entry.config = areq.config;
        code = 200;
        msg = ublkd_msg_added(id, "/dev/ublkb" + std::to_string(id));
        LOG_INFO("ublkd: added /dev/ublkb` image `", id, areq.config.c_str());
    }

    void handle_del(const std::string &body, int &code, std::string &msg) {
        int id = -1;
        std::string err;
        if (ublkd_parse_del(body, id, err) != 0) {
            code = 400;
            msg = ublkd_msg_error(err);
            return;
        }
        auto it = devices_.find(id);
        if (it == devices_.end()) {
            code = 404;
            msg = ublkd_msg_error("no such device: " + std::to_string(id));
            return;
        }
        // Deletion state machine (M2-2): del is synchronous and yields while
        // draining, so a concurrent del of the same id would otherwise race
        // on the map iterator (erase invalidates the other handler's it).
        if (it->second.stopping) {
            code = 409;
            msg = ublkd_msg_error("delete of device " + std::to_string(id) +
                                  " is already in progress");
            return;
        }
        it->second.stopping = true;
        it->second.dev->stop();
        it->second.dev->wait();
        it->second.dev->teardown();
        devices_.erase(id); // re-lookup by key: `it` may not survive yields
        code = 200;
        msg = ublkd_msg_ok();
        LOG_INFO("ublkd: deleted /dev/ublkb`", id);
    }

    void handle_list(int &code, std::string &msg) {
        std::vector<UblkdDeviceInfo> infos;
        for (auto &kv : devices_) {
            UblkdDeviceInfo info;
            info.dev_id = kv.first;
            info.dev_path = "/dev/ublkb" + std::to_string(kv.first);
            info.config = kv.second.config;
            info.writable = kv.second.dev->writable();
            info.state = kv.second.stopping ? "stopping" : "running";
            infos.push_back(std::move(info));
        }
        code = 200;
        msg = ublkd_msg_list(infos);
    }

    ImageService *service_;
    std::string cache_base_;
    struct DevEntry {
        std::unique_ptr<UblkDevice> dev;
        std::string config;
        bool stopping = false;
    };
    std::map<int, DevEntry> devices_;
    // realpath'd image configs with an add in progress (UX guard, see above)
    std::set<std::string> adds_in_flight_;
};

// entry-layer signal routing (same pattern & rationale as cli g_signal_device)
static bool *g_shutdown_flag = nullptr;

static void sigterm_handler(int signal) {
    LOG_INFO("ublkd: signal ` received, shutting down", signal);
    if (g_shutdown_flag != nullptr)
        *g_shutdown_flag = true;
}

int main(int argc, char **argv) {
    std::string socket_path = UBLKD_DEFAULT_SOCK;
    std::string service_config;
    std::string cache_dir;

    CLI::App app{"overlaybd-ublkd: centralized ublk device manager daemon "
                 "(all devices in one process, shared ImageService)"};
    app.add_option("--socket-path", socket_path,
                   "unix socket for the control API (default " UBLKD_DEFAULT_SOCK ")");
    app.add_option("--service-config", service_config,
                   "overlaybd service config (default /etc/overlaybd/overlaybd.json)");
    app.add_option("--cache-dir", cache_dir,
                   "cache base directory (default /opt/overlaybd/ublk_cache); "
                   "the daemon uses <base>/daemon/ for all devices and ignores "
                   "the service config's cacheDir");
    CLI11_PARSE(app, argc, argv);

    if (ublk_check_control_dev() != 0)
        return 1;
    mkdir(OVERLAYBD_UBLK_RUN_DIR, 0755);

    // daemon hygiene: park stdin on /dev/null so fd 0 can never be reused by
    // resource fds (a queue io_uring landing on fd 0 was observed once after
    // stdin vanished mid-session; any code treating fd 0/1/2 specially then
    // corrupts it). stdout/stderr stay attached for foreground logs.
    int devnull = open("/dev/null", O_RDWR);
    if (devnull > 0) {
        dup2(devnull, 0);
        close(devnull);
    }

    // shared cache tree + daemon flock + patched service config (M2-1);
    // plain fs/flock work, safe before photon::init
    std::string patched_config;
    int cache_lock_fd = -1; // held until exit; kernel releases on any death
    if (ublk_daemon_setup_cache(cache_dir, service_config, patched_config,
                                cache_lock_fd) != 0)
        return 1;

    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    photon::block_all_signal();
    photon::sync_signal(SIGTERM, &sigterm_handler);
    photon::sync_signal(SIGINT, &sigterm_handler);

    ImageService *service = create_image_service(patched_config.c_str());
    if (service == nullptr) {
        unlink(patched_config.c_str());
        fprintf(stderr, "overlaybd-ublkd: failed to create image service\n");
        return 1;
    }
    // patched copy stays for the daemon's lifetime: create_image_file
    // re-parses this path for the global default download section
    // photon LOG prints adjacent string literals with quotes, keep one literal
    LOG_INFO("cache: `/daemon (service config cacheDir is overridden, see --cache-dir)",
             cache_dir.empty() ? "/opt/overlaybd/ublk_cache" : cache_dir.c_str());

    auto *server = new UblkdServer(service, cache_dir);
    g_shutdown_flag = &server->shutdown_requested;

    auto *sock = photon::net::new_uds_server(true /* autoremove */);
    if (sock->bind(socket_path.c_str()) != 0 || sock->listen() != 0) {
        fprintf(stderr, "overlaybd-ublkd: cannot listen on %s: %s\n",
                socket_path.c_str(), strerror(errno));
        return 1;
    }
    chmod(socket_path.c_str(), 0600); // root-only control surface
    auto *http = photon::net::http::new_http_server();
    http->add_handler(server, false, "/v1");
    sock->set_handler(http->get_connection_handler());
    sock->start_loop(false);
    LOG_INFO("overlaybd-ublkd ready, control socket: `", socket_path.c_str());

    // main coroutine idles here; signal handlers and connection handlers run
    // as photon coroutines in this thread
    while (!server->shutdown_requested)
        photon::thread_usleep(200 * 1000);

    LOG_INFO("overlaybd-ublkd: draining devices and exiting");
    sock->terminate();
    delete http;
    delete server; // parallel-stops and tears down every device
    delete sock;
    delete service;
    unlink(patched_config.c_str());
    photon::fini();
    return 0;
}
