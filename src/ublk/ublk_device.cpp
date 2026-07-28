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
#include "ublk_device.h"
#include "config_patch.h"
#include "io_dispatch.h"

#include "../image_file.h"
#include "../image_service.h"

#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/io/fd-events.h>
#include <photon/io/signal.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread-pool.h>

#include <ublksrv.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <climits>
#include <unistd.h>

int ublk_check_control_dev() {
    if (access(UBLK_CONTROL_DEV, F_OK) == 0)
        return 0;
    fprintf(stderr,
            "overlaybd-ublk: %s not found.\n"
            "The ublk frontend requires the ublk_drv kernel driver "
            "(mainline kernel >= 6.0, or a distro kernel with ublk backported).\n"
            "Try: modprobe ublk_drv\n",
            UBLK_CONTROL_DEV);
    return -1;
}

// ImageFile adapter over the UblkIOTarget seam. Reads carry the same
// retry-forever semantics as the TCMU frontend's sure() helper; writes
// are single-shot (EROFS etc. must surface immediately).
class ImageFileTarget : public UblkIOTarget {
public:
    explicit ImageFileTarget(ImageFile *file) : m_file(file) {
    }

    ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override {
        auto time_st = photon::now;
        uint64_t try_cnt = 0, sleep_period = 20UL * 1000;
    again:
        if (photon::now - time_st > 7LL * 24 * 60 * 60 * 1000 * 1000 /*7days*/) {
            LOG_ERROR_RETURN(EIO, -1, "sure request timeout, offset: `", offset);
        }
        ssize_t ret = m_file->preadv(iov, iovcnt, offset);
        if (ret >= 0) {
            return ret;
        }
        if (try_cnt % 10 == 0) {
            LOG_ERROR("io request failed, offset: `, ret: `, retry times: `, errno:`", offset,
                      ret, try_cnt, errno);
        }
        try_cnt++;
        photon::thread_usleep(sleep_period);
        sleep_period = std::min(sleep_period * 2, 30UL * 1000 * 1000);
        goto again;
    }

    ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override {
        return m_file->pwritev(iov, iovcnt, offset);
    }

    int fdatasync() override {
        return m_file->fdatasync();
    }

    int fallocate(int mode, off_t offset, off_t len) override {
        return m_file->fallocate(mode, offset, len);
    }

private:
    ImageFile *m_file;
};

// ---------------------------------------------------------------------------
// UblkDevice small members (the class declaration lives in ublk_device.h;
// one instance == one device, N instances per process is the daemon mode)
// ---------------------------------------------------------------------------

void UblkDevice::stop() {
    if (ctrl_dev_ != nullptr)
        ublksrv_ctrl_stop_dev(ctrl_dev_);
}

uint64_t UblkDevice::image_size() const {
    return file_->num_lbas * (uint64_t)file_->block_size;
}

bool UblkDevice::writable() const {
    return file_ != nullptr && !file_->read_only;
}

UblkDevice::~UblkDevice() {
    // safety net for the daemon path; the normal orders are run(), or
    // stop() -> wait() -> teardown() -- every step below is idempotent
    stop();
    wait();
    teardown();
    if (cache_lock_fd_ >= 0)
        close(cache_lock_fd_);
}

// ---------------------------------------------------------------------------
// data plane: per-queue photon-owned hybrid event loop
// ---------------------------------------------------------------------------

struct QueueCtx {
    const struct ublksrv_queue *q = nullptr;
    photon::ThreadPool<32> *pool = nullptr;
    UblkIOTarget *target = nullptr;
    uint32_t inflight = 0;
};

struct IOJob {
    const struct ublksrv_queue *q;
    const struct ublk_io_data *data;
};

static void *io_worker(void *arg) {
    IOJob *job = (IOJob *)arg;
    auto *ctx = (QueueCtx *)job->q->private_data;
    const struct ublksrv_io_desc *iod = job->data->iod;

    int res = ublk_dispatch_io(ctx->target, ublksrv_get_op(iod), iod->start_sector << 9,
                               iod->nr_sectors << 9,
                               ublksrv_queue_get_io_buf(job->q, job->data->tag));

    ublksrv_complete_io(job->q, job->data->tag, res);
    // Pitfall: complete_io only queues the COMMIT SQE; nobody else
    // submits it in this integration mode, so submit right here (same thread
    // as the queue loop, no eventfd needed).
    io_uring_submit(job->q->ring_ptr);

    ctx->inflight--;
    delete job;
    return nullptr;
}

static int obd_handle_io_async(const struct ublksrv_queue *q, const struct ublk_io_data *data) {
    auto *ctx = (QueueCtx *)q->private_data;
    ctx->inflight++;
    ctx->pool->thread_create(&io_worker, new IOJob{q, data});
    return 0;
}

static int obd_init_tgt(struct ublksrv_dev *dev, int type, int argc, char *argv[]) {
    const struct ublksrv_ctrl_dev *cdev = ublksrv_get_ctrl_dev(dev);
    const struct ublksrv_ctrl_dev_info *info = ublksrv_ctrl_get_dev_info(cdev);
    // the owning UblkDevice travels through the official priv-data slot
    auto *device = (UblkDevice *)ublksrv_ctrl_get_priv_data(cdev);
    dev->tgt.dev_size = device->image_size();
    dev->tgt.tgt_ring_depth = info->queue_depth;
    dev->tgt.nr_fds = 0;
    return 0;
}

static struct ublksrv_tgt_type obd_tgt_type = {
    .handle_io_async = obd_handle_io_async,
    .init_tgt = obd_init_tgt,
    .name = "overlaybd",
};

// Probe IORING_SETUP_COOP_TASKRUN (mainline 5.19+) support once per process
// with a private throwaway ring. Letting ublksrv_queue_init_flags fail and
// retrying without the flag looks harmless, but libublksrv's failure path
// runs ublksrv_queue_deinit on a zero-initialized queue whose epollfd/efd
// are 0 behind `>= 0` guards -- it closes fd 0. With one process per device
// the victim is just stdin (and the retried ring lands on fd 0, working by
// luck); in the daemon, fd 0 IS the previous device's queue ring, so every
// add silently wedged the device added before it. Never let the first
// attempt fail.
static unsigned probe_queue_ring_flags() {
    struct io_uring probe;
    int r = io_uring_queue_init(2, &probe, IORING_SETUP_COOP_TASKRUN);
    if (r == 0) {
        io_uring_queue_exit(&probe);
        return IORING_SETUP_COOP_TASKRUN;
    }
    LOG_WARN("IORING_SETUP_COOP_TASKRUN unsupported (`), queues run without it", r);
    return 0;
}

// The queue thread owns its own photon env and acts as the event-loop master:
// poll the io_uring ring fd via photon epoll, reap CQEs only when readable
// (never enter ublksrv_process_io's blocking wait), submit queued SQEs after
// each round. This is safe with ublksrv v1.7: ring_ptr is a public field,
// reap_events is a public zero-wait API, and submit_and_wait returns
// immediately when the CQ is non-empty.
//
// Runtime-verified on a 5.10 kernel with ublk backported (startup sequence,
// idle wakeup, full-queue fio pressure); a regression pass on mainline 6.x
// kernels is still pending.
void UblkDevice::queue_loop(const struct ublksrv_dev *dev, photon::semaphore *started,
                            int *init_ret) {
    photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_LIBCURL);
    DEFER(photon::fini());

    photon::ThreadPool<32> pool;
    QueueCtx ctx;
    ctx.pool = &pool;
    ctx.target = target_;

    const struct ublksrv_queue *q =
        ublksrv_queue_init_flags(dev, 0, &ctx, ring_flags_);
    if (q == nullptr) {
        // no blind retry: every failed init runs the library's broken
        // cleanup path (closes fd 0), see probe_queue_ring_flags()
        LOG_ERROR("ublksrv_queue_init failed (see syslog for libublksrv details)");
        *init_ret = -1;
        queue_exited_.store(true);
        started->signal(1);
        return;
    }
    ctx.q = q;

    // flush the initial FETCH SQEs prepared by ublksrv_queue_init, otherwise
    // the kernel never sees them and the loop below waits forever
    io_uring_submit(q->ring_ptr);

    *init_ret = 0;
    started->signal(1);

    while (true) {
        int ret = photon::wait_for_fd_readable(q->ring_ptr->ring_fd, 1000UL * 1000);
        if (ret == 0) {
            // CQEs pending: reap without waiting; this drives handle_io_async
            // and the internal queue state machine (STOPPING etc.)
            ublksrv_queue_reap_events(q);
        } else if (errno != ETIMEDOUT) {
            LOG_ERROR("wait on ring fd failed: `", strerror(errno));
            break;
        }
        // submit FETCH/COMMIT SQEs queued during the reap round
        io_uring_submit(q->ring_ptr);

        if ((ublksrv_queue_state(q) & UBLKSRV_QUEUE_STOPPING) && ctx.inflight == 0)
            break;
    }

    // Drain phase: official daemons only deinit after ublksrv_process_io()
    // returns -ENODEV, i.e. every io command (FETCH) has been completed or
    // canceled by the kernel (cmd_inflight == 0, tracked inside libublksrv).
    // Closing the ring with uring_cmds still inflight leaks ublk device
    // references on kernels lacking uring_cmd cancel-on-exit (observed on the
    // 5.10 backport), which makes the later UBLK_CMD_DEL_DEV wait forever in
    // ublk_ctrl_del_dev. Blocking here is fine: all target IO has finished,
    // only cancellation CQEs remain.
    while (ublksrv_process_io(q) >= 0)
        ;

    LOG_INFO("ublk queue exited");
    ublksrv_queue_deinit(q);
    queue_exited_.store(true);
}

// ---------------------------------------------------------------------------
// control plane: ctrl dev lifecycle, owned by the main thread
// ---------------------------------------------------------------------------

void UblkDevice::set_dev_parameters() {
    const struct ublksrv_ctrl_dev_info *info = ublksrv_ctrl_get_dev_info(ctrl_dev_);
    uint8_t bs_shift = (uint8_t)__builtin_ctz(file_->block_size);

    struct ublk_params p;
    memset(&p, 0, sizeof(p));
    p.types = UBLK_PARAM_TYPE_BASIC | UBLK_PARAM_TYPE_DISCARD;
    // honest cache declaration: writable images have a volatile
    // cache (FLUSH -> ImageFile::fdatasync); read-only images have nothing
    // volatile to flush, and advertising a cache would make the kernel send
    // FLUSH that fdatasync() on a RO ImageFile fails with (dmesg:
    // "lost sync page write"), so declare no cache for them
    if (file_->read_only)
        p.basic.attrs = UBLK_ATTR_READ_ONLY;
    else
        p.basic.attrs = UBLK_ATTR_VOLATILE_CACHE;
    p.basic.logical_bs_shift = bs_shift;
    p.basic.physical_bs_shift = 12;
    p.basic.io_opt_shift = 12;
    p.basic.io_min_shift = bs_shift;
    p.basic.max_sectors = info->max_io_buf_bytes >> 9;
    p.basic.dev_sectors = image_size() >> 9;
    // TCMU equivalent: tcmu_dev_set_unmap_enabled(true)
    p.discard.discard_granularity = file_->block_size;
    p.discard.max_discard_sectors = info->max_io_buf_bytes >> 9;
    p.discard.max_write_zeroes_sectors = info->max_io_buf_bytes >> 9;
    p.discard.max_discard_segments = 1;

    int ret = ublksrv_ctrl_set_params(ctrl_dev_, &p);
    if (ret)
        LOG_ERROR("ublk dev ` set params failed: `", info->dev_id, ret);
}

static void notify_ready(int ready_fd, int dev_id) {
    if (ready_fd < 0)
        return;
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "/dev/ublkb%d\n", dev_id);
    ssize_t nwritten = write(ready_fd, buf, len);
    if (nwritten != len)
        LOG_ERROR("failed to notify readiness: `", strerror(errno));
    if (ready_fd > 2)
        close(ready_fd);
}

// failure counterpart of the sync-ready contract: the daemonized child has
// no usable stderr and alog is not yet file-backed during early setup, so
// the reason travels to the add command's console through the same pipe
static void notify_error(int ready_fd, const std::string &reason) {
    if (ready_fd <= 2) // foreground mode already printed to a live stderr
        return;
    std::string msg =
        "ERR:" +
        (reason.empty() ? std::string("unknown error, check the overlaybd log")
                        : reason) +
        "\n";
    ssize_t nwritten = write(ready_fd, msg.data(), msg.size());
    (void)nwritten;
    close(ready_fd);
}

// Delete the ublk device and release the ctrl dev, without ever blocking
// this process forever. Sync DEL_DEV waits in-kernel until every device
// reference is dropped; on some kernels (observed: 5.10 backport) the last
// reference only goes away at process exit, turning the sync wait into a
// deadlock. So: prefer DEL_DEV_ASYNC (kernel 6.5+); otherwise run sync del
// on a helper thread with a bounded wait, and if it is still blocked just
// exit -- process teardown drops the references and the kernel completes
// the pending deletion (verified: a hung DEL finished exactly at process
// death and the dev id was reclaimed).
void UblkDevice::ctrl_del_and_deinit() {
    int ret = ublksrv_ctrl_del_dev_async(ctrl_dev_);
    if (ret < 0) {
        LOG_WARN("DEL_DEV_ASYNC unavailable (`), falling back to bounded sync del", ret);
        auto *done = new std::atomic<bool>(false);
        struct ublksrv_ctrl_dev *cdev = ctrl_dev_;
        std::thread del_thread([done, cdev] {
            ublksrv_ctrl_del_dev(cdev);
            done->store(true);
        });
        for (int i = 0; i < 20 && !done->load(); i++)
            photon::thread_usleep(100 * 1000); // up to 2s: healthy kernels finish in ms
        if (!done->load()) {
            // photon LOG macros print adjacent string literals with quotes, keep one literal
            LOG_WARN("DEL_DEV still blocked after 2s, exiting anyway; process teardown releases the last device references and the kernel finishes the deletion");
            del_thread.detach();
            // deliberately leak `done` and skip ctrl_deinit: the ctrl ring is
            // still in use by the blocked command, process exit reclaims both
            ctrl_dev_ = nullptr;
            return;
        }
        del_thread.join();
        delete done;
    }
    ublksrv_ctrl_deinit(ctrl_dev_);
    ctrl_dev_ = nullptr;
}

// mkdir -p: ImageService's create_dir only does single-level mkdir, so a
// fresh --cache-dir path must be created (with subdirs) before the service
// initializes.
static int mkdir_p(const std::string &path, mode_t mode) {
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            std::string cur = path.substr(0, i);
            if (mkdir(cur.c_str(), mode) != 0 && errno != EEXIST)
                return -1;
        }
    }
    return 0;
}

// --cache-dir support: ImageService parses its cache directories deep inside
// init(), so the override is applied by writing a patched copy of the service
// config into the run dir and pointing create_image_service at it (keeps the
// shared image_service code untouched). The temp file is removed right after
// the service is created.
static int make_patched_service_config(const std::string &service_config_path,
                                       const std::string &cache_root,
                                       std::string &out_path) {
    const char *base_path = service_config_path.empty()
                                ? "/etc/overlaybd/overlaybd.json" // create_image_service default
                                : service_config_path.c_str();
    std::ifstream in(base_path);
    if (!in) {
        fprintf(stderr, "overlaybd-ublk: cannot read service config %s\n", base_path);
        return -1;
    }
    std::string base_json((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());

    std::string patched;
    if (ublk_patch_cache_dirs(base_json, cache_root, patched) != 0) {
        fprintf(stderr, "overlaybd-ublk: service config %s is not a valid JSON object\n",
                base_path);
        return -1;
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%d.service.json", OVERLAYBD_UBLK_RUN_DIR,
             (int)getpid());
    std::ofstream out(path, std::ios::trunc);
    if (!out || !(out << patched)) {
        fprintf(stderr, "overlaybd-ublk: cannot write %s\n", path);
        return -1;
    }
    out_path = path;
    return 0;
}

// Cache isolation is mandatory: the file cache's eviction/refill locking is
// in-process only, so two daemons sharing a directory can corrupt accounting
// or serve stale zeroes. Layout: <base>/<image-key>/<instance>/, where the
// image key follows the image identity (realpath of its config) so the same
// image reuses a warm cache, and the instance is claimed via flock -- the
// same read-only image mounted N times gets N isolated instances (inst0,
// inst1, ...), while writable images are restricted to inst0 (concurrent RW
// mounts would corrupt the upper layer).

static bool valid_instance_id(const std::string &s) {
    if (s.empty() || s.size() > 64)
        return false;
    for (char c : s)
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-')
            return false;
    return true;
}

// lock <dir>/.lock exclusively: 0 = locked (fd_out valid, keep for the
// holder's lifetime; kernel releases on any process exit), -2 = busy,
// -1 = hard error
static int lock_dir(const std::string &dir, int &fd_out) {
    if (mkdir_p(dir, 0755) != 0) {
        fprintf(stderr, "overlaybd-ublk: cannot create %s: %s\n", dir.c_str(),
                strerror(errno));
        return -1;
    }
    std::string lock_path = dir + "/.lock";
    int fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "overlaybd-ublk: cannot open %s: %s\n", lock_path.c_str(),
                strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -2;
    }
    fd_out = fd;
    return 0;
}

// claim <image_root>/<inst>: 0 = claimed, -2 = busy, -1 = hard error
int UblkDevice::claim_instance(const std::string &image_root, const std::string &inst,
                               std::string &cache_root) {
    std::string root = image_root + "/" + inst;
    if (mkdir_p(root + "/registry_cache", 0755) != 0 ||
        mkdir_p(root + "/gzip_cache", 0755) != 0) {
        fprintf(stderr, "overlaybd-ublk: cannot create cache dir %s: %s\n", root.c_str(),
                strerror(errno));
        return -1;
    }
    int r = lock_dir(root, cache_lock_fd_);
    if (r != 0)
        return r;
    cache_root = root;
    return 0;
}

int UblkDevice::setup_cache_root(const UblkDeviceOpts &opts, std::string &cache_root) {
    char resolved[PATH_MAX];
    if (realpath(opts.image_config_path.c_str(), resolved) == nullptr) {
        last_error_ = "image config " + opts.image_config_path + ": " + strerror(errno);
        fprintf(stderr, "overlaybd-ublk: %s\n", last_error_.c_str());
        return -1;
    }

    std::ifstream img_in(resolved);
    std::string image_json((std::istreambuf_iterator<char>(img_in)),
                           std::istreambuf_iterator<char>());
    bool writable = ublk_config_has_upper(image_json);

    std::string base =
        opts.cache_dir.empty() ? "/opt/overlaybd/ublk_cache" : opts.cache_dir;
    std::string image_root = base + "/" + ublk_image_cache_key(resolved);
    if (mkdir_p(image_root, 0755) != 0) {
        fprintf(stderr, "overlaybd-ublk: cannot create %s: %s\n", image_root.c_str(),
                strerror(errno));
        return -1;
    }
    // best effort: record which image this cache belongs to
    std::ofstream src(image_root + "/source", std::ios::trunc);
    if (src)
        src << resolved << "\n";
    src.close();

    if (!opts.instance_id.empty()) {
        if (writable) {
            last_error_ = "--instance-id is not allowed for a writable image "
                          "(it can only be mounted once)";
            fprintf(stderr, "overlaybd-ublk: %s\n", last_error_.c_str());
            return -1;
        }
        if (!valid_instance_id(opts.instance_id)) {
            last_error_ = "invalid --instance-id (allowed: [A-Za-z0-9._-], "
                          "max 64 chars)";
            fprintf(stderr, "overlaybd-ublk: %s\n", last_error_.c_str());
            return -1;
        }
        int r = claim_instance(image_root, opts.instance_id, cache_root);
        if (r == -2) {
            last_error_ = "instance '" + opts.instance_id +
                          "' of this image is already running";
            fprintf(stderr, "overlaybd-ublk: %s\n", last_error_.c_str());
        } else if (r == -1) {
            last_error_ = "cannot create cache directories under " + image_root;
        }
        return r == 0 ? 0 : -1;
    }

    if (writable) {
        int r = claim_instance(image_root, "inst0", cache_root);
        if (r == -2) {
            last_error_ = "writable image already mounted by another daemon "
                          "or CLI process (concurrent RW mounts would corrupt "
                          "the upper layer)";
            fprintf(stderr, "overlaybd-ublk: %s\n", last_error_.c_str());
        } else if (r == -1) {
            last_error_ = "cannot create cache directories under " + image_root;
        }
        return r == 0 ? 0 : -1;
    }

    for (int i = 0; i < 64; i++) {
        int r = claim_instance(image_root, "inst" + std::to_string(i), cache_root);
        if (r == 0)
            return 0;
        if (r == -1) {
            last_error_ = "cannot create cache directories under " + image_root;
            return -1;
        }
    }
    last_error_ = "all 64 cache instance slots of this image are busy";
    fprintf(stderr, "overlaybd-ublk: %s\n", last_error_.c_str());
    return -1;
}

// CLI path only: mandatory cache isolation (ADR-0004) + own ImageService
int UblkDevice::init_service(const UblkDeviceOpts &opts) {
    std::string cache_root;
    if (setup_cache_root(opts, cache_root) != 0)
        return -1;
    std::string patched_config; // temp file, removed after service init
    if (make_patched_service_config(opts.service_config_path, cache_root,
                                    patched_config) != 0) {
        last_error_ = "cannot read or patch the service config";
        return -1;
    }

    imgservice_ = create_image_service(patched_config.c_str());
    if (imgservice_ == nullptr) {
        unlink(patched_config.c_str());
        last_error_ = "failed to create image service (bad service config?)";
        LOG_ERROR("failed to create image service");
        return -1;
    }
    // keep the patched copy until teardown: create_image_file re-parses the
    // service config path later (global default download section); deleting
    // it here silently drops those defaults (seen as "default download
    // config parse failed" warnings)
    patched_config_path_ = patched_config;
    owns_service_ = true;
    // per-device log: redirect after create_image_service (which applied the
    // shared logPath from the global config); reuse the global size/rotation
    if (!opts.log_path.empty()) {
        // APPCFG defaults (10MB x 3) also cover old-style configs without logConfig
        uint64_t log_size = 1024UL * 1024 * imgservice_->global_conf.logConfig().logSizeMB();
        int ret = log_output_file(opts.log_path.c_str(), log_size,
                                  imgservice_->global_conf.logConfig().logRotateNum());
        if (ret != 0)
            LOG_ERROR("failed to set per-device log path `, keep shared log",
                      opts.log_path.c_str());
        else
            LOG_INFO("per-device log: `", opts.log_path.c_str());
    }
    return 0;
}

// start() failed mid-way: remove whatever was created so that neither the
// kernel nor this object keeps a half-created device (ADR-0006 hard rule)
void UblkDevice::cleanup_failed_start(bool dev_added) {
    if (ctrl_dev_ != nullptr) {
        if (dev_added) {
            ctrl_del_and_deinit();
        } else {
            ublksrv_ctrl_deinit(ctrl_dev_);
            ctrl_dev_ = nullptr;
        }
    }
    delete target_;
    target_ = nullptr;
    delete file_;
    file_ = nullptr;
    dev_id_ = -1;
}

int ublk_daemon_setup_cache(const std::string &cache_base,
                            const std::string &service_config_path,
                            std::string &patched_config, int &lock_fd) {
    std::string base =
        cache_base.empty() ? "/opt/overlaybd/ublk_cache" : cache_base;
    std::string daemon_root = base + "/daemon";
    // "daemon" can never collide with a CLI image key (16 hex chars)
    if (mkdir_p(daemon_root + "/registry_cache", 0755) != 0 ||
        mkdir_p(daemon_root + "/gzip_cache", 0755) != 0) {
        fprintf(stderr, "overlaybd-ublkd: cannot create cache dir %s: %s\n",
                daemon_root.c_str(), strerror(errno));
        return -1;
    }
    int r = lock_dir(daemon_root, lock_fd);
    if (r == -2) {
        fprintf(stderr,
                "overlaybd-ublkd: cache dir %s is held by another daemon; "
                "run a second instance with a different --cache-dir (and "
                "--socket-path)\n",
                daemon_root.c_str());
        return -1;
    }
    if (r != 0)
        return -1;
    return make_patched_service_config(service_config_path, daemon_root,
                                       patched_config);
}

// Daemon path: cross-process exclusion for writable images. The daemon
// shares one cache tree, so it takes no per-image cache instance -- but a
// writable image mounted here AND by a single-device CLI process would
// still corrupt the upper layer. Reuse the ADR-0004 lock file
// (<base>/<image-key>/inst0/.lock) as lock-only, no cache subdirs, so both
// worlds exclude each other as long as they share the cache base.
int UblkDevice::acquire_rw_image_lock(const UblkDeviceOpts &opts) {
    char resolved[PATH_MAX];
    if (realpath(opts.image_config_path.c_str(), resolved) == nullptr) {
        LOG_ERROR("realpath(`) failed: `", opts.image_config_path.c_str(),
                  strerror(errno));
        return -1;
    }
    std::ifstream in(resolved);
    std::string image_json((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    if (!ublk_config_has_upper(image_json))
        return 0; // read-only: shared cache, in-process locks suffice

    std::string base =
        opts.cache_dir.empty() ? "/opt/overlaybd/ublk_cache" : opts.cache_dir;
    std::string inst0 = base + "/" + ublk_image_cache_key(resolved) + "/inst0";
    int r = lock_dir(inst0, cache_lock_fd_);
    if (r == -2) {
        LOG_ERROR("writable image ` already mounted (another daemon or CLI "
                  "process holds `/.lock)",
                  resolved, inst0.c_str());
        return -1;
    }
    return r == 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------
// raw ublk control commands (M2-3)
// ---------------------------------------------------------------------------
// libublksrv v1.7 wraps GET_FEATURES but not UPDATE_SIZE, and its generic
// sender is private -- so send the uring_cmd ourselves on a throwaway ring
// against /dev/ublk-control, assembled exactly like ublksrv_ctrl_init_cmd
// (SQE128, payload at sqe->addr3, cmd op at sqe->off). Resize is rare, the
// per-call ring setup cost is irrelevant.
static int ublk_raw_ctrl(uint32_t cmd_op, int dev_id, uint64_t data0, void *buf,
                         uint16_t len) {
    int cfd = open(UBLK_CONTROL_DEV, O_RDWR);
    if (cfd < 0)
        return -errno;
    struct io_uring ring;
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_SQE128;
    int ret = io_uring_queue_init_params(4, &ring, &p);
    if (ret < 0) {
        close(cfd);
        return ret;
    }
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    memset(sqe, 0, 128); // SQE128 slot
    sqe->fd = cfd;
    sqe->opcode = IORING_OP_URING_CMD;
    *(uint32_t *)&sqe->off = cmd_op; // ublksrv_set_sqe_cmd_op equivalent
    struct ublksrv_ctrl_cmd *cmd = (struct ublksrv_ctrl_cmd *)&sqe->addr3;
    cmd->dev_id = dev_id;
    cmd->queue_id = (uint16_t)-1;
    cmd->data[0] = data0;
    cmd->addr = (uint64_t)buf;
    cmd->len = len;
    do {
        ret = io_uring_submit_and_wait(&ring, 1);
    } while (ret == -EINTR);
    if (ret >= 0) {
        struct io_uring_cqe *cqe = nullptr;
        ret = io_uring_peek_cqe(&ring, &cqe);
        if (ret == 0) {
            ret = cqe->res;
            io_uring_cqe_seen(&ring, cqe);
        }
    }
    io_uring_queue_exit(&ring);
    close(cfd);
    return ret;
}

static int ublk_raw_ctrl_cmd(uint32_t cmd_op, int dev_id, uint64_t data0) {
    return ublk_raw_ctrl(cmd_op, dev_id, data0, nullptr, 0);
}

static int ublk_raw_ctrl_cmd_buf(uint32_t cmd_op, int dev_id, void *buf, uint16_t len) {
    return ublk_raw_ctrl(cmd_op, dev_id, 0, buf, len);
}

// UBLK_F_UPDATE_SIZE support, probed once per process (GET_FEATURES itself
// is 6.5+; on kernels without it the probe fails -> no support)
static bool kernel_supports_update_size() {
    static int cached = -1;
    if (cached < 0) {
        uint64_t features = 0;
        int r = ublk_raw_ctrl_cmd_buf(UBLK_U_CMD_GET_FEATURES, -1, &features,
                                      sizeof(features));
        cached = (r >= 0 && (features & UBLK_F_UPDATE_SIZE)) ? 1 : 0;
        if (!cached)
            LOG_WARN("UBLK_F_UPDATE_SIZE unavailable (probe `), online resize disabled", r);
    }
    return cached == 1;
}

int UblkDevice::resize(uint64_t new_size_bytes, bool resize_fs, std::string &err) {
    if (!writable()) {
        err = "device is read-only; resize applies to writable images only";
        return -3;
    }
    if (new_size_bytes <= image_size()) {
        err = "new size must be larger than the current " +
              std::to_string(image_size() >> 30) + " GiB (only growing is supported)";
        return -3;
    }
    if (!kernel_supports_update_size()) {
        err = "kernel lacks UBLK_F_UPDATE_SIZE (mainline >= 6.11 required)";
        return -2;
    }
    // image layer first (and ext4 if asked): on failure nothing changed
    if (file_->resize(new_size_bytes, resize_fs) != 0) {
        err = "image resize failed (see the overlaybd log)";
        return -1;
    }
    // then tell the kernel; sectors of 512
    int r = ublk_raw_ctrl_cmd(UBLK_U_CMD_UPDATE_SIZE, dev_id_, new_size_bytes >> 9);
    if (r < 0) {
        err = "UBLK_U_CMD_UPDATE_SIZE failed (" + std::to_string(r) +
              "); image already grown, kernel size unchanged";
        return -1;
    }
    LOG_INFO("resized /dev/ublkb` to ` bytes (resize_fs=`)", dev_id_, new_size_bytes,
             resize_fs);
    return 0;
}

int UblkDevice::start(const UblkDeviceOpts &opts) {
    if (imgservice_ == nullptr) {
        LOG_ERROR("no ImageService bound (daemon must inject one)");
        return -1;
    }
    // daemon path: writable images need cross-process exclusion before the
    // upper layer is opened (CLI takes its lock in setup_cache_root instead)
    if (!owns_service_ && acquire_rw_image_lock(opts) != 0)
        return -1;
    // registration tag: ImageService keys its device table by this string,
    // so it must be unique. CLI: pid alone (one device per process, matches
    // the log tag). Daemon: many devices share the pid, append a sequence --
    // a colliding tag fails the second create_image_file outright
    // (register_image_file: "dev id exists").
    std::string dev_tag = "ublk-" + std::to_string(getpid());
    if (!owns_service_) {
        static std::atomic<int> seq{0};
        dev_tag += "-" + std::to_string(seq.fetch_add(1));
    }
    // open the image before anything ublk-related: dev_size is needed by
    // init_tgt, and a broken config must fail the add command, not the device
    file_ = imgservice_->create_image_file(opts.image_config_path.c_str(), dev_tag);
    if (file_ == nullptr) {
        last_error_ = "failed to open image " + opts.image_config_path +
                      " (bad image config or unreachable layer files)";
        LOG_ERROR("failed to open image `", opts.image_config_path.c_str());
        return -1;
    }
    target_ = new ImageFileTarget(file_);

    // probe once, main-thread serialized, before any queue thread exists
    ring_flags_ = probe_queue_ring_flags();

    struct ublksrv_dev_data data;
    memset(&data, 0, sizeof(data));
    data.dev_id = opts.dev_id;
    data.max_io_buf_bytes = 512 * 1024;
    data.nr_hw_queues = 1; // v1: single queue, one event-loop thread
    data.queue_depth = (unsigned short)opts.queue_depth;
    data.tgt_type = "overlaybd";
    data.tgt_ops = &obd_tgt_type;
    data.run_dir = OVERLAYBD_UBLK_RUN_DIR;
    // request online-resize capability when the kernel has it (the
    // UPDATE_SIZE command only works on devices created with the flag)
    data.flags = kernel_supports_update_size() ? UBLK_F_UPDATE_SIZE : 0;

    ctrl_dev_ = ublksrv_ctrl_init(&data);
    if (ctrl_dev_ == nullptr) {
        LOG_ERROR("ublksrv_ctrl_init failed");
        cleanup_failed_start(false);
        return -1;
    }
    // route the owning device into libublksrv callbacks (obd_init_tgt)
    ublksrv_ctrl_set_priv_data(ctrl_dev_, this);

    int ret = ublksrv_ctrl_add_dev(ctrl_dev_);
    if (ret < 0) {
        LOG_ERROR("ublksrv_ctrl_add_dev failed: `", ret);
        last_error_ = "UBLK ADD_DEV failed (" + std::to_string(ret) +
                      "); requested dev id busy or kernel refused";
        cleanup_failed_start(false);
        return -1;
    }
    dev_id_ = ublksrv_ctrl_get_dev_info(ctrl_dev_)->dev_id;

    ret = ublksrv_ctrl_get_affinity(ctrl_dev_);
    if (ret < 0)
        LOG_WARN("ublksrv_ctrl_get_affinity failed: `", ret);

    dev_ = ublksrv_dev_init(ctrl_dev_);
    if (dev_ == nullptr) {
        LOG_ERROR("ublksrv_dev_init failed");
        cleanup_failed_start(true);
        return -1;
    }

    photon::semaphore queue_started;
    int queue_init_ret = -1;
    queue_thread_ = std::thread(&UblkDevice::queue_loop, this, dev_, &queue_started,
                                &queue_init_ret);
    queue_started.wait(1);
    if (queue_init_ret != 0) {
        queue_thread_.join();
        ublksrv_dev_deinit(dev_);
        dev_ = nullptr;
        last_error_ = "queue ring init failed (see overlaybd log / syslog)";
        cleanup_failed_start(true);
        return -1;
    }

    set_dev_parameters();

    ret = ublksrv_ctrl_start_dev(ctrl_dev_, getpid());
    if (ret < 0) {
        LOG_ERROR("ublksrv_ctrl_start_dev failed: `", ret);
        ublksrv_ctrl_stop_dev(ctrl_dev_); // unblock the queue thread
        while (!queue_exited_.load())
            photon::thread_usleep(10 * 1000);
        queue_thread_.join();
        ublksrv_dev_deinit(dev_);
        dev_ = nullptr;
        cleanup_failed_start(true);
        return -1;
    }

    LOG_INFO("overlaybd-ublk device /dev/ublkb` ready, image: `", dev_id_,
             opts.image_config_path.c_str());
    return 0;
}

void UblkDevice::wait() {
    if (!queue_thread_.joinable())
        return;
    // photon-sleep instead of a bare join: photon coroutines of this thread
    // (signal handlers, daemon control requests) must keep getting scheduled
    while (!queue_exited_.load())
        photon::thread_usleep(200 * 1000);
    queue_thread_.join();
    if (dev_ != nullptr) {
        ublksrv_dev_deinit(dev_);
        dev_ = nullptr;
    }
}

void UblkDevice::teardown() {
    if (torn_down_)
        return;
    torn_down_ = true;
    if (ctrl_dev_ != nullptr)
        ctrl_del_and_deinit();
    delete target_;
    target_ = nullptr;
    delete file_;
    file_ = nullptr;
    if (owns_service_)
        delete imgservice_;
    imgservice_ = nullptr;
    if (!patched_config_path_.empty()) {
        unlink(patched_config_path_.c_str());
        patched_config_path_.clear();
    }
}

int UblkDevice::run(const UblkDeviceOpts &opts, int ready_fd) {
    if (ublk_check_control_dev() != 0)
        return 1;
    mkdir(OVERLAYBD_UBLK_RUN_DIR, 0755); // pidfile dir for libublksrv

    if (init_service(opts) != 0) {
        notify_error(ready_fd, last_error_);
        return 1;
    }
    if (start(opts) != 0) {
        notify_error(ready_fd, last_error_);
        teardown();
        return 1;
    }
    notify_ready(ready_fd, dev_id_);
    wait();
    teardown();
    LOG_INFO("overlaybd-ublk exited");
    return 0;
}

// ---------------------------------------------------------------------------
// standalone-binary entry layer
// ---------------------------------------------------------------------------

// Signal routing for the one-device-per-process binary: POSIX handlers can't
// carry a closure, so the current device is parked in a file-scope pointer.
// This is entry-layer plumbing, not device state -- a future daemon
// (ADR-0005) installs its own handler that walks its device table instead.
static UblkDevice *g_signal_device = nullptr;

static void stop_device_handler(int signal) {
    LOG_INFO("signal ` received, stopping ublk device", signal);
    if (g_signal_device != nullptr)
        g_signal_device->stop();
}

int run_ublk_device(const UblkDeviceOpts &opts, int ready_fd) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    photon::block_all_signal();
    photon::sync_signal(SIGTERM, &stop_device_handler);
    photon::sync_signal(SIGINT, &stop_device_handler);

    UblkDevice device;
    g_signal_device = &device;
    int ret = device.run(opts, ready_fd);
    g_signal_device = nullptr;
    return ret;
}
