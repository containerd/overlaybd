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
#pragma once

#include <atomic>
#include <string>
#include <thread>

// pidfiles (<dev_id>.pid, created by libublksrv) live here; `del` and `list`
// discover overlaybd-ublk daemons through this directory only.
#define OVERLAYBD_UBLK_RUN_DIR "/var/run/overlaybd-ublk"
#define UBLK_CONTROL_DEV "/dev/ublk-control"

struct UblkDeviceOpts {
    std::string image_config_path;   // overlaybd image config (config.v1.json)
    std::string service_config_path; // global service config, empty = default
    // per-device log file; empty = shared log from the global service config.
    // With one process per device, daemons sharing one log file interleave
    // indistinguishable lines and race on rotation, so a dedicated path per
    // device is recommended when running multiple devices.
    std::string log_path;
    // base directory for this device's cache; empty = /opt/overlaybd/ublk_cache.
    // Cache isolation is always on: caches live in <base>/<image-key>/<instance>/
    // (key = hash of the image config's realpath) guarded by an exclusive
    // flock, because the file cache's locking is in-process only.
    std::string cache_dir;
    // explicit cache instance name; empty = auto slot (inst0, inst1, ...).
    // Lets callers with a stable identity (e.g. a future snapshotter) pin
    // an instance; only meaningful for read-only images.
    std::string instance_id;
    int dev_id = -1;                 // -1: let the kernel allocate
    int queue_depth = 128;
};

// Check /dev/ublk-control and report a clear error if unusable.
// Returns 0 on success, -1 (with message on stderr) otherwise.
int ublk_check_control_dev();

// Run one overlaybd-ublk device until SIGTERM/SIGINT (process == device).
// On readiness the block device path ("/dev/ublkbN\n") is
// written to ready_fd (the add command's sync-ready contract); ready_fd
// is closed afterwards unless it is stdout/stderr. Returns the process
// exit code.
int run_ublk_device(const UblkDeviceOpts &opts, int ready_fd);

// Daemon-side cache setup (ADR-0006 M2-1). All devices of a daemon share
// one cache tree <base>/daemon/{registry_cache,gzip_cache}: creates it,
// takes an exclusive flock on <base>/daemon/.lock (a second daemon on the
// same base is refused), and writes a service config copy patched to that
// tree (the cacheDir of the original service config is deliberately
// ignored -- single-valued rule, and the default keeps clear of tcmu's
// /opt/overlaybd/registry_cache). Returns 0 and the lock fd (held for the
// daemon's lifetime) or -1 with a message on stderr.
int ublk_daemon_setup_cache(const std::string &cache_base,
                            const std::string &service_config_path,
                            std::string &patched_config, int &lock_fd);

class ImageService;
class ImageFile;
class ImageFileTarget;
struct ublksrv_ctrl_dev;
struct ublksrv_dev;
namespace photon {
class semaphore;
}

// One ublk device: image open, ctrl dev lifecycle, queue event-loop thread.
// Two construction paths (ADR-0006):
//  - CLI (default ctor + run()): owns its ImageService, with the mandatory
//    per-image cache isolation of ADR-0004 (image key + instance slots);
//  - daemon (inject a shared ImageService + start/wait/stop): cache patching
//    and instance slots are skipped -- all devices of the daemon share one
//    cache directory, whose concurrency is covered by in-process locks.
class UblkDevice {
public:
    UblkDevice() = default;
    explicit UblkDevice(ImageService *shared_service)
        : imgservice_(shared_service), owns_service_(false) {
    }
    ~UblkDevice();

    UblkDevice(const UblkDevice &) = delete;
    UblkDevice &operator=(const UblkDevice &) = delete;

    // CLI blocking path: init_service + start + readiness + wait + teardown.
    int run(const UblkDeviceOpts &opts, int ready_fd);

    // Daemon path. start() brings the device up (0 = ready, dev_id() valid);
    // a partial device is cleaned up internally on failure (no leftovers in
    // the kernel). wait() blocks until the queue exits and releases the
    // queue/dev resources; teardown() deletes the ublk device and the image.
    int start(const UblkDeviceOpts &opts);
    void wait();
    void stop(); // request stop; safe from a photon signal coroutine
    void teardown();

    // Online grow (M2-3): image layer (ImageFile::resize, optionally ext4)
    // + kernel layer (UBLK_U_CMD_UPDATE_SIZE). Writable images only; the
    // kernel must support UBLK_F_UPDATE_SIZE (mainline >= 6.11).
    // Returns 0 = ok; -1 = internal error; -2 = kernel lacks the feature;
    // -3 = invalid request (read-only device / shrink). err says why.
    int resize(uint64_t new_size_bytes, bool resize_fs, std::string &err);

    int dev_id() const {
        return dev_id_;
    }
    bool writable() const;
    uint64_t image_size() const; // needed by the init_tgt callback

private:
    int init_service(const UblkDeviceOpts &opts); // CLI path only
    void cleanup_failed_start(bool dev_added);    // no half-created device stays
    int setup_cache_root(const UblkDeviceOpts &opts, std::string &cache_root);
    int acquire_rw_image_lock(const UblkDeviceOpts &opts); // daemon path only
    int claim_instance(const std::string &image_root, const std::string &inst,
                       std::string &cache_root);
    void set_dev_parameters();
    void ctrl_del_and_deinit();
    void queue_loop(const struct ublksrv_dev *dev, photon::semaphore *started,
                    int *init_ret);

    ImageService *imgservice_ = nullptr;
    ImageFile *file_ = nullptr;
    ImageFileTarget *target_ = nullptr;
    struct ublksrv_ctrl_dev *ctrl_dev_ = nullptr;
    const struct ublksrv_dev *dev_ = nullptr;
    std::thread queue_thread_;
    std::atomic<bool> queue_exited_{false};
    unsigned ring_flags_ = 0; // queue io_uring flags, probed in start()
    int cache_lock_fd_ = -1; // held for the device's lifetime
    // human-readable reason of a failed bring-up; relayed to the add
    // command's console through the ready pipe (daemonized childs lose
    // stderr, and alog is not yet redirected during early setup)
    std::string last_error_;
    // CLI path: patched service config path, kept alive until teardown
    // (create_image_file re-parses it for the global download defaults)
    std::string patched_config_path_;
    int dev_id_ = -1;
    bool owns_service_ = true;
    bool torn_down_ = false;
};
