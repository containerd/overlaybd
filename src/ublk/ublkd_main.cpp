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

// overlaybd-ublkd: the centralized ublk device manager daemon.
// All devices live in this process (one queue thread each) and share one
// ImageService; control plane is HTTP over a unix domain socket
// (docker.sock style), requests are handled globally serialized.

#include "ublk_device.h"
#include "ublkd_protocol.h"
#include "pool_placeholder.h"
#include "blkdev_hygiene.h"

#include "../image_service.h"
#include "../image_file.h"
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
    struct DevEntry {
        std::unique_ptr<UblkDevice> dev;
        std::string config;
        bool stopping = false;
        bool mode_shared = false; // acquired shared (refcounted) vs add/exclusive
        int refcount = 1;         // meaningful when mode_shared
        bool pooled = false;      // came from the warm pool, can be recycled
        int pool_slot = -1;       // which placeholder image to swap back in
    };

    UblkdServer(ImageService *service, std::string cache_base)
        : service_(service), cache_base_(std::move(cache_base)) {
    }

    // Shared read-only device leases: off unless enabled. When disabled,
    // acquire(mode=shared) is not an error -- it degrades to an independent
    // device per caller (the same thing add would give), and the response
    // reports mode "exclusive" so the caller can tell sharing did not happen.
    void enable_shared_devices(bool on) {
        shared_devices_enabled_ = on;
    }

    // warm pool: off unless low > 0. Pooled devices are pre-created on
    // daemon-owned placeholder images and handed out by hot-swapping the real
    // image in, saving the ~8ms device-shell creation. Only writable images
    // are pooled for now: the attrs set at creation time (block size, RO/RW)
    // cannot change afterwards.
    void configure_pool(int low, int high, uint64_t size_gb) {
        pool_low_ = low;
        pool_high_ = high > low ? high : low;
        pool_size_gb_ = size_gb;
    }

    void prewarm_pool() {
        refill_pool();
    }

    ~UblkdServer() {
        // stop all devices in parallel: STOP first, join afterwards, so the
        // total teardown time is about the slowest device instead of the sum
        // (the bounded-DEL fallback costs 2s per device on backport kernels)
        for (auto &kv : devices_)
            kv.second.dev->stop();
        for (auto &p : pool_idle_)
            p.dev->stop();
        for (auto &kv : devices_) {
            kv.second.dev->wait();
            kv.second.dev->teardown();
        }
        for (auto &p : pool_idle_) {
            p.dev->wait();
            p.dev->teardown();
            delete p.dev;
        }
        pool_idle_.clear();
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
        } else if (target == "/v1/acquire" &&
                   req.verb() == photon::net::http::Verb::POST) {
            handle_acquire(body, code, msg);
        } else if (target == "/v1/release" &&
                   req.verb() == photon::net::http::Verb::POST) {
            handle_release(body, code, msg);
        } else if (target == "/v1/resize" && req.verb() == photon::net::http::Verb::POST) {
            handle_resize(body, code, msg);
        } else if (target == "/v1/list") {
            handle_list(code, msg);
        } else if (target == "/v1/pool") {
            code = 200;
            msg = ublkd_msg_pool(pool_low_, pool_high_, (int)pool_idle_.size(),
                                 pool_size_gb_, pool_hits_, pool_misses_);
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
        // Per-image in-flight guard. Note: the same-image RW race is already
        // closed by the inst0 flock (LOCK_EX excludes between fds of one
        // process too); this guard is UX -- reject the
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
        // device where the cross-process RW image locks live
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
        // del is for exclusive (add-created) devices; shared devices are
        // reference-counted and must go through release, or a caller would
        // rip a device out from under other holders
        if (it->second.mode_shared) {
            code = 409;
            msg = ublkd_msg_error("device " + std::to_string(id) +
                                  " is shared (refcount " +
                                  std::to_string(it->second.refcount) +
                                  "); use /v1/release");
            return;
        }
        // Deletion state machine: del is synchronous and yields while
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

    void handle_resize(const std::string &body, int &code, std::string &msg) {
        UblkdResizeRequest rreq;
        std::string err;
        if (ublkd_parse_resize(body, rreq, err) != 0) {
            code = 400;
            msg = ublkd_msg_error(err);
            return;
        }
        auto it = devices_.find(rreq.dev_id);
        if (it == devices_.end()) {
            code = 404;
            msg = ublkd_msg_error("no such device: " + std::to_string(rreq.dev_id));
            return;
        }
        if (it->second.stopping) {
            code = 409;
            msg = ublkd_msg_error("device is being deleted");
            return;
        }
        int r = it->second.dev->resize(rreq.size_gb << 30, rreq.resize_fs, err);
        if (r == 0) {
            code = 200;
            msg = ublkd_msg_ok();
            LOG_INFO("ublkd: resized dev ` to ` GB", rreq.dev_id, rreq.size_gb);
        } else {
            code = (r == -2) ? 501 : (r == -3) ? 400 : 500;
            msg = ublkd_msg_error(err);
        }
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
            info.mode = kv.second.mode_shared ? "shared" : "exclusive";
            info.refcount = kv.second.mode_shared ? kv.second.refcount : 0;
            infos.push_back(std::move(info));
        }
        code = 200;
        msg = ublkd_msg_list(infos);
    }

    // acquire: lease semantics. shared -> reuse one device per image
    // (refcounted); exclusive -> a fresh device (equivalent to add unless the
    // warm pool is enabled). Only read-only images may be shared.
    void handle_acquire(const std::string &body, int &code, std::string &msg) {
        UblkdAcquireRequest areq;
        std::string err;
        if (ublkd_parse_acquire(body, areq, err) != 0) {
            code = 400;
            msg = ublkd_msg_error(err);
            return;
        }
        char resolved[PATH_MAX];
        if (realpath(areq.config.c_str(), resolved) == nullptr) {
            code = 400;
            msg = ublkd_msg_error("image config " + areq.config + ": " +
                                  strerror(errno));
            return;
        }
        // shared leases only when the feature is enabled; otherwise the
        // request degrades to an independent device (see enable_shared_devices)
        bool shared = (areq.mode == "shared") && shared_devices_enabled_;
        if (areq.mode == "shared" && !shared_devices_enabled_)
            LOG_INFO("ublkd: shared device leases disabled, serving `"
                     " an independent device",
                     areq.config.c_str());

        // shared: an already-running device for this image gets one more ref
        if (shared) {
            auto sit = shared_.find(resolved);
            if (sit != shared_.end()) {
                auto dit = devices_.find(sit->second);
                if (dit != devices_.end() && !dit->second.stopping) {
                    dit->second.refcount++;
                    code = 200;
                    msg = ublkd_msg_acquired(sit->second,
                                             "/dev/ublkb" + std::to_string(sit->second),
                                             "shared", dit->second.refcount);
                    LOG_INFO("ublkd: acquire(shared) hit dev ` refcount `",
                             sit->second, dit->second.refcount);
                    return;
                }
                if (dit != devices_.end() && dit->second.stopping) {
                    code = 409;
                    msg = ublkd_msg_error("shared device for this image is being "
                                          "torn down; retry");
                    return;
                }
                shared_.erase(sit); // stale mapping, fall through to create
            }
        }

        if (adds_in_flight_.count(resolved)) {
            code = 409;
            msg = ublkd_msg_error("an acquire/add of this image is already in progress");
            return;
        }
        adds_in_flight_.insert(resolved);

        // warm pool fast path: exclusive leases may come from the pool
        std::unique_ptr<UblkDevice> dev;
        bool from_pool = false;
        int pool_slot = -1;
        if (!shared && pool_low_ > 0 && !pool_idle_.empty()) {
            ImageFile *real = service_->create_image_file(areq.config.c_str(),
                                                         next_dev_tag());
            if (real == nullptr) {
                adds_in_flight_.erase(resolved);
                code = 500;
                msg = ublkd_msg_error("cannot open image (see daemon log)");
                return;
            }
            UblkDevice *pooled = take_pooled(real, &pool_slot);
            if (pooled != nullptr) {
                dev.reset(pooled);
                from_pool = true;
                pool_hits_++;
            } else {
                delete real; // no pooled device matched; fall back to a new one
                pool_misses_++;
            }
        } else if (!shared && pool_low_ > 0) {
            pool_misses_++;
        }

        if (!from_pool) {
            dev = std::make_unique<UblkDevice>(service_);
            UblkDeviceOpts opts;
            opts.image_config_path = areq.config;
            opts.cache_dir = cache_base_;
            if (dev->start(opts) != 0) {
                adds_in_flight_.erase(resolved);
                code = 500;
                msg = ublkd_msg_error("device failed to start (see daemon log)");
                return;
            }
        }
        adds_in_flight_.erase(resolved);
        // shared mode requires a read-only image (concurrent RW would corrupt
        // the upper); reject after start so writable() is known, then tear down
        if (shared && dev->writable()) {
            dev->stop();
            dev->wait();
            dev->teardown();
            code = 400;
            msg = ublkd_msg_error("cannot share a writable image (only read-only "
                                  "images may be acquired shared)");
            return;
        }
        int id = dev->dev_id();
        auto &entry = devices_[id];
        entry.dev = std::move(dev);
        entry.config = areq.config;
        entry.mode_shared = shared;
        entry.refcount = 1;
        entry.pooled = from_pool;
        entry.pool_slot = pool_slot;
        if (shared)
            shared_[resolved] = id;
        code = 200;
        msg = ublkd_msg_acquired(id, "/dev/ublkb" + std::to_string(id),
                                 shared ? "shared" : "exclusive", 1);
        LOG_INFO("ublkd: acquired /dev/ublkb` mode ` pooled ` image `", id,
                 shared ? "shared" : "exclusive", from_pool ? 1 : 0,
                 areq.config.c_str());
        if (pool_low_ > 0)
            refill_pool();
    }

    void handle_release(const std::string &body, int &code, std::string &msg) {
        int id = -1;
        std::string err;
        if (ublkd_parse_del(body, id, err) != 0) { // same body shape as del
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
        if (it->second.stopping) {
            code = 409;
            msg = ublkd_msg_error("device " + std::to_string(id) +
                                  " is being torn down");
            return;
        }
        if (--it->second.refcount > 0) {
            code = 200;
            msg = ublkd_msg_released(it->second.refcount);
            LOG_INFO("ublkd: release dev ` refcount `", id, it->second.refcount);
            return;
        }
        // last reference (or an exclusive lease): back to the pool if it came
        // from there and can be recycled safely, otherwise tear it down
        if (it->second.mode_shared) {
            char resolved[PATH_MAX];
            if (realpath(it->second.config.c_str(), resolved) != nullptr)
                shared_.erase(resolved);
        }
        if (it->second.pooled && return_to_pool(id, it->second)) {
            devices_.erase(id);
            code = 200;
            msg = ublkd_msg_released(0);
            LOG_INFO("ublkd: released /dev/ublkb` back to the pool", id);
            return;
        }
        it->second.stopping = true;
        it->second.dev->stop();
        it->second.dev->wait();
        it->second.dev->teardown();
        devices_.erase(id);
        code = 200;
        msg = ublkd_msg_released(0);
        LOG_INFO("ublkd: released and removed /dev/ublkb`", id);
    }

    // ---- warm pool internals ------------------------------------------------

    std::string next_dev_tag() {
        return "ublk-" + std::to_string(getpid()) + "-p" + std::to_string(tag_seq_++);
    }

    // Take an idle pooled device whose fixed attributes match the image and
    // hot-swap the image in. Returns nullptr when nothing matches (caller
    // falls back to creating a device). Ownership of `real` transfers on
    // success; on failure the caller releases it.
    UblkDevice *take_pooled(ImageFile *real, int *slot_out) {
        for (auto it = pool_idle_.begin(); it != pool_idle_.end(); ++it) {
            UblkDevice *cand = it->dev;
            // fixed-at-creation attributes must match (see the pool key)
            if (cand->block_size() != real->block_size)
                continue;
            if (cand->dev_sectors() !=
                (real->num_lbas * (uint64_t)real->block_size) >> 9)
                continue;
            if (!cand->writable()) // first version pools writable devices only
                continue;
            ImageFile *old_file = nullptr;
            ImageFileTarget *old_target = nullptr;
            auto *new_target = ublk_make_image_target(real);
            if (cand->swap_image(real, new_target, &old_file, &old_target) != 0) {
                delete new_target;
                // a device we cannot swap is not trustworthy: drop it
                int slot = it->slot;
                pool_idle_.erase(it);
                cand->stop();
                cand->wait();
                cand->teardown();
                delete cand;
                free_slots_.push_back(slot);
                return nullptr;
            }
            *slot_out = it->slot;
            pool_idle_.erase(it);
            delete old_target;
            delete old_file; // placeholder image released
            return cand;
        }
        return nullptr;
    }

    // Recycle a device: invalidate the block device's page cache, swap the
    // placeholder image back in, and park it. Any doubt -> return false and
    // let the caller tear the device down (safety over reuse).
    bool return_to_pool(int dev_id, DevEntry &entry) {
        if ((int)pool_idle_.size() >= pool_high_)
            return false;
        if (ublk_device_is_mounted(dev_id)) {
            LOG_WARN("dev ` still mounted, not recycling it", dev_id);
            return false;
        }
        if (ublk_device_flush_buffers(dev_id) != 0)
            return false; // stale pages could leak to the next tenant
        std::string ph_config;
        if (ublk_pool_placeholder(placeholder_dir(), pool_size_gb_, entry.pool_slot,
                                  ph_config) != 0)
            return false;
        ImageFile *ph = service_->create_image_file(ph_config.c_str(), next_dev_tag());
        if (ph == nullptr)
            return false;
        ImageFile *old_file = nullptr;
        ImageFileTarget *old_target = nullptr;
        auto *ph_target = ublk_make_image_target(ph);
        UblkDevice *dev = entry.dev.release();
        if (dev->swap_image(ph, ph_target, &old_file, &old_target) != 0) {
            delete ph_target;
            delete ph;
            entry.dev.reset(dev); // give it back so the caller tears it down
            return false;
        }
        delete old_target;
        delete old_file; // the tenant's image
        pool_idle_.push_back({dev, entry.pool_slot});
        return true;
    }

    std::string placeholder_dir() const {
        std::string base =
            (cache_base_.empty() ? "/opt/overlaybd/ublk_cache" : cache_base_) +
            "/daemon/placeholders";
        mkdir(base.c_str(), 0755);
        return base;
    }

    // Bring the idle pool up to the low watermark. Synchronous and
    // best-effort: creating a device is ~8ms, and this runs after a request
    // has already been answered.
    void refill_pool() {
        while ((int)pool_idle_.size() < pool_low_) {
            // each pooled device needs its own placeholder files
            int slot;
            if (!free_slots_.empty()) {
                slot = free_slots_.back();
                free_slots_.pop_back();
            } else {
                slot = next_slot_++;
            }
            std::string ph_config;
            if (ublk_pool_placeholder(placeholder_dir(), pool_size_gb_, slot,
                                      ph_config) != 0) {
                free_slots_.push_back(slot);
                return;
            }
            auto *dev = new UblkDevice(service_);
            UblkDeviceOpts opts;
            opts.image_config_path = ph_config;
            opts.cache_dir = cache_base_;
            if (dev->start(opts) != 0) {
                delete dev;
                free_slots_.push_back(slot);
                LOG_WARN("pool refill: device start failed, pool stays at `",
                         (int)pool_idle_.size());
                return;
            }
            LOG_INFO("pool refill: /dev/ublkb` parked (slot `, idle `)",
                     dev->dev_id(), slot, (int)pool_idle_.size() + 1);
            pool_idle_.push_back({dev, slot});
        }
    }

    ImageService *service_;
    std::string cache_base_;
    std::map<int, DevEntry> devices_;
    // realpath(image config) -> dev_id of its shared device
    std::map<std::string, int> shared_;
    // realpath'd image configs with an add in progress (UX guard, see above)
    std::set<std::string> adds_in_flight_;
    // warm pool: idle devices parked on placeholder images
    struct PooledDev {
        UblkDevice *dev;
        int slot; // its own placeholder file set
    };
    std::vector<PooledDev> pool_idle_;
    std::vector<int> free_slots_; // slots of devices that went away
    int next_slot_ = 0;
    bool shared_devices_enabled_ = false; // --enable-shared-devices
    int pool_low_ = 0; // 0 = pooling disabled
    int pool_high_ = 0;
    uint64_t pool_size_gb_ = 0;
    uint64_t pool_hits_ = 0, pool_misses_ = 0;
    uint64_t tag_seq_ = 0;
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
    int pool_low = 0, pool_high = 0;
    uint64_t pool_size_gb = 0;
    bool enable_shared = false;
    app.add_flag("--enable-shared-devices", enable_shared,
                 "let /v1/acquire with mode=shared hand the SAME device to "
                 "every consumer of a read-only image (reference counted). "
                 "Off by default: shared requests then get an independent "
                 "device each, exactly like add");
    app.add_option("--pool-low", pool_low,
                   "warm pool: keep at least N idle devices ready (0 = pooling "
                   "disabled, the default); exclusive acquires are then served "
                   "by hot-swapping the image into a pre-created device");
    app.add_option("--pool-high", pool_high,
                   "warm pool: never park more than N idle devices (default = low)");
    app.add_option("--pool-size-gb", pool_size_gb,
                   "warm pool: virtual size of pooled devices in GB; only images "
                   "of exactly this size (and matching block size, writable) can "
                   "be served from the pool");
    CLI11_PARSE(app, argc, argv);

    if (pool_low > 0 && pool_size_gb == 0) {
        fprintf(stderr, "overlaybd-ublkd: --pool-low requires --pool-size-gb\n");
        return 1;
    }

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

    // shared cache tree + daemon flock + patched service config;
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
    server->enable_shared_devices(enable_shared);
    LOG_INFO("shared read-only device leases: `",
             enable_shared ? "enabled" : "disabled (each shared acquire gets "
                                         "its own device)");
    if (pool_low > 0) {
        server->configure_pool(pool_low, pool_high, pool_size_gb);
        server->prewarm_pool();
        LOG_INFO("warm pool enabled: low ` high ` size ` GB", pool_low,
                 pool_high > pool_low ? pool_high : pool_low, pool_size_gb);
    }

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
