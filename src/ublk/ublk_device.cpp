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
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

// Single device per process (the frontend's core design decision), so
// file-scope state is fine, mirroring the tcmu frontend's main.cpp style.
static ImageService *g_imgservice = nullptr;
static ImageFile *g_file = nullptr;
static struct ublksrv_ctrl_dev *g_ctrl_dev = nullptr;
static std::atomic<bool> g_queue_exited{false};

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

static ImageFileTarget *g_target = nullptr;

// ---------------------------------------------------------------------------
// data plane: per-queue photon-owned hybrid event loop
// ---------------------------------------------------------------------------

struct QueueCtx {
    const struct ublksrv_queue *q = nullptr;
    photon::ThreadPool<32> *pool = nullptr;
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

    int res = ublk_dispatch_io(g_target, ublksrv_get_op(iod), iod->start_sector << 9,
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
    const struct ublksrv_ctrl_dev_info *info =
        ublksrv_ctrl_get_dev_info(ublksrv_get_ctrl_dev(dev));
    dev->tgt.dev_size = g_file->num_lbas * g_file->block_size;
    dev->tgt.tgt_ring_depth = info->queue_depth;
    dev->tgt.nr_fds = 0;
    return 0;
}

static struct ublksrv_tgt_type obd_tgt_type = {
    .handle_io_async = obd_handle_io_async,
    .init_tgt = obd_init_tgt,
    .name = "overlaybd",
};

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
static void queue_thread_fn(const struct ublksrv_dev *dev, photon::semaphore *started,
                            int *init_ret) {
    photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_LIBCURL);
    DEFER(photon::fini());

    photon::ThreadPool<32> pool;
    QueueCtx ctx;
    ctx.pool = &pool;

    // IORING_SETUP_COOP_TASKRUN (mainline 5.19+) is a pure optimization, but
    // ublksrv_queue_init() hardcodes it. Backport kernels shipping ublk_drv on
    // older baselines (e.g. Alinux 5.10.134) may lack it and fail with EINVAL,
    // so retry without the flag before giving up.
    const struct ublksrv_queue *q =
        ublksrv_queue_init_flags(dev, 0, &ctx, IORING_SETUP_COOP_TASKRUN);
    if (q == nullptr) {
        LOG_WARN("queue init with IORING_SETUP_COOP_TASKRUN failed, retrying without it");
        q = ublksrv_queue_init_flags(dev, 0, &ctx, 0);
    }
    if (q == nullptr) {
        LOG_ERROR("ublksrv_queue_init failed (see syslog for libublksrv details)");
        *init_ret = -1;
        g_queue_exited.store(true);
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
    g_queue_exited.store(true);
}

// ---------------------------------------------------------------------------
// control plane: ctrl dev lifecycle, owned by the main thread
// ---------------------------------------------------------------------------

static void set_dev_parameters(struct ublksrv_ctrl_dev *cdev, ImageFile *file) {
    const struct ublksrv_ctrl_dev_info *info = ublksrv_ctrl_get_dev_info(cdev);
    uint8_t bs_shift = (uint8_t)__builtin_ctz(file->block_size);

    struct ublk_params p;
    memset(&p, 0, sizeof(p));
    p.types = UBLK_PARAM_TYPE_BASIC | UBLK_PARAM_TYPE_DISCARD;
    // honest cache declaration: writable images have a volatile
    // cache (FLUSH -> ImageFile::fdatasync); read-only images have nothing
    // volatile to flush, and advertising a cache would make the kernel send
    // FLUSH that fdatasync() on a RO ImageFile fails with (dmesg:
    // "lost sync page write"), so declare no cache for them
    if (file->read_only)
        p.basic.attrs = UBLK_ATTR_READ_ONLY;
    else
        p.basic.attrs = UBLK_ATTR_VOLATILE_CACHE;
    p.basic.logical_bs_shift = bs_shift;
    p.basic.physical_bs_shift = 12;
    p.basic.io_opt_shift = 12;
    p.basic.io_min_shift = bs_shift;
    p.basic.max_sectors = info->max_io_buf_bytes >> 9;
    p.basic.dev_sectors = (file->num_lbas * file->block_size) >> 9;
    // TCMU equivalent: tcmu_dev_set_unmap_enabled(true)
    p.discard.discard_granularity = file->block_size;
    p.discard.max_discard_sectors = info->max_io_buf_bytes >> 9;
    p.discard.max_write_zeroes_sectors = info->max_io_buf_bytes >> 9;
    p.discard.max_discard_segments = 1;

    int ret = ublksrv_ctrl_set_params(cdev, &p);
    if (ret)
        LOG_ERROR("ublk dev ` set params failed: `", info->dev_id, ret);
}

static void stop_device_handler(int signal) {
    LOG_INFO("signal ` received, stopping ublk device", signal);
    if (g_ctrl_dev != nullptr)
        ublksrv_ctrl_stop_dev(g_ctrl_dev);
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

// Delete the ublk device and release the ctrl dev, without ever blocking
// this process forever. Sync DEL_DEV waits in-kernel until every device
// reference is dropped; on some kernels (observed: 5.10 backport) the last
// reference only goes away at process exit, turning the sync wait into a
// deadlock. So: prefer DEL_DEV_ASYNC (kernel 6.5+); otherwise run sync del
// on a helper thread with a bounded wait, and if it is still blocked just
// exit -- process teardown drops the references and the kernel completes
// the pending deletion (verified: a hung DEL finished exactly at process
// death and the dev id was reclaimed).
static void ctrl_del_and_deinit() {
    int ret = ublksrv_ctrl_del_dev_async(g_ctrl_dev);
    if (ret < 0) {
        LOG_WARN("DEL_DEV_ASYNC unavailable (`), falling back to bounded sync del", ret);
        auto *done = new std::atomic<bool>(false);
        struct ublksrv_ctrl_dev *cdev = g_ctrl_dev;
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
            g_ctrl_dev = nullptr;
            return;
        }
        del_thread.join();
        delete done;
    }
    ublksrv_ctrl_deinit(g_ctrl_dev);
    g_ctrl_dev = nullptr;
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
static int make_patched_service_config(const UblkDeviceOpts &opts, std::string &out_path) {
    const char *base_path = opts.service_config_path.empty()
                                ? "/etc/overlaybd/overlaybd.json" // create_image_service default
                                : opts.service_config_path.c_str();
    std::ifstream in(base_path);
    if (!in) {
        fprintf(stderr, "overlaybd-ublk: cannot read service config %s\n", base_path);
        return -1;
    }
    std::string base_json((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());

    std::string patched;
    if (ublk_patch_cache_dirs(base_json, opts.cache_dir, patched) != 0) {
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

int run_ublk_device(const UblkDeviceOpts &opts, int ready_fd) {
    if (ublk_check_control_dev() != 0)
        return 1;
    mkdir(OVERLAYBD_UBLK_RUN_DIR, 0755); // pidfile dir for libublksrv

    std::string service_config = opts.service_config_path;
    std::string patched_config; // temp file, removed after service init
    if (!opts.cache_dir.empty()) {
        if (mkdir_p(opts.cache_dir + "/registry_cache", 0755) != 0 ||
            mkdir_p(opts.cache_dir + "/gzip_cache", 0755) != 0) {
            fprintf(stderr, "overlaybd-ublk: cannot create cache dir %s: %s\n",
                    opts.cache_dir.c_str(), strerror(errno));
            return 1;
        }
        if (make_patched_service_config(opts, patched_config) != 0)
            return 1;
        service_config = patched_config;
    }

    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    photon::block_all_signal();
    photon::sync_signal(SIGTERM, &stop_device_handler);
    photon::sync_signal(SIGINT, &stop_device_handler);

    g_imgservice = create_image_service(
        service_config.empty() ? nullptr : service_config.c_str());
    if (!patched_config.empty())
        unlink(patched_config.c_str()); // config parsed, temp copy no longer needed
    if (g_imgservice == nullptr) {
        LOG_ERROR("failed to create image service");
        return 1;
    }
    // per-device log: redirect after create_image_service (which applied the
    // shared logPath from the global config); reuse the global size/rotation
    if (!opts.log_path.empty()) {
        // APPCFG defaults (10MB x 3) also cover old-style configs without logConfig
        uint64_t log_size = 1024UL * 1024 * g_imgservice->global_conf.logConfig().logSizeMB();
        int ret = log_output_file(opts.log_path.c_str(), log_size,
                                  g_imgservice->global_conf.logConfig().logRotateNum());
        if (ret != 0)
            LOG_ERROR("failed to set per-device log path `, keep shared log",
                      opts.log_path.c_str());
        else
            LOG_INFO("per-device log: `", opts.log_path.c_str());
    }
    // tag with pid so that daemons sharing one log remain distinguishable
    std::string dev_tag = "ublk-" + std::to_string(getpid());
    // open the image before anything ublk-related: dev_size is needed by
    // init_tgt, and a broken config must fail the add command, not the device
    g_file = g_imgservice->create_image_file(opts.image_config_path.c_str(), dev_tag);
    if (g_file == nullptr) {
        LOG_ERROR("failed to open image `", opts.image_config_path.c_str());
        return 1;
    }
    g_target = new ImageFileTarget(g_file);

    struct ublksrv_dev_data data;
    memset(&data, 0, sizeof(data));
    data.dev_id = opts.dev_id;
    data.max_io_buf_bytes = 512 * 1024;
    data.nr_hw_queues = 1; // v1: single queue, one event-loop thread
    data.queue_depth = (unsigned short)opts.queue_depth;
    data.tgt_type = "overlaybd";
    data.tgt_ops = &obd_tgt_type;
    data.run_dir = OVERLAYBD_UBLK_RUN_DIR;
    data.flags = 0;

    g_ctrl_dev = ublksrv_ctrl_init(&data);
    if (g_ctrl_dev == nullptr) {
        LOG_ERROR("ublksrv_ctrl_init failed");
        return 1;
    }

    int ret = ublksrv_ctrl_add_dev(g_ctrl_dev);
    if (ret < 0) {
        LOG_ERROR("ublksrv_ctrl_add_dev failed: `", ret);
        ublksrv_ctrl_deinit(g_ctrl_dev);
        return 1;
    }
    int dev_id = ublksrv_ctrl_get_dev_info(g_ctrl_dev)->dev_id;

    ret = ublksrv_ctrl_get_affinity(g_ctrl_dev);
    if (ret < 0)
        LOG_WARN("ublksrv_ctrl_get_affinity failed: `", ret);

    {
        const struct ublksrv_dev *dev = ublksrv_dev_init(g_ctrl_dev);
        if (dev == nullptr) {
            LOG_ERROR("ublksrv_dev_init failed");
            goto fail_del_dev;
        }

        photon::semaphore queue_started;
        int queue_init_ret = -1;
        std::thread queue_thread(queue_thread_fn, dev, &queue_started, &queue_init_ret);
        queue_started.wait(1);
        if (queue_init_ret != 0) {
            queue_thread.join();
            ublksrv_dev_deinit(dev);
            goto fail_del_dev;
        }

        set_dev_parameters(g_ctrl_dev, g_file);

        ret = ublksrv_ctrl_start_dev(g_ctrl_dev, getpid());
        if (ret < 0) {
            LOG_ERROR("ublksrv_ctrl_start_dev failed: `", ret);
            ublksrv_ctrl_stop_dev(g_ctrl_dev); // unblock the queue thread
            while (!g_queue_exited.load())
                photon::thread_usleep(10 * 1000);
            queue_thread.join();
            ublksrv_dev_deinit(dev);
            goto fail_del_dev;
        }

        LOG_INFO("overlaybd-ublk device /dev/ublkb` ready, image: `", dev_id,
                 opts.image_config_path.c_str());
        notify_ready(ready_fd, dev_id);

        // photon-sleep instead of a bare join: sync_signal handlers run as
        // photon coroutines in this thread and must keep getting scheduled
        while (!g_queue_exited.load())
            photon::thread_usleep(200 * 1000);
        queue_thread.join();
        ublksrv_dev_deinit(dev);
    }

    ctrl_del_and_deinit();

    delete g_target;
    delete g_file;
    delete g_imgservice;
    LOG_INFO("overlaybd-ublk exited");
    return 0;

fail_del_dev:
    ctrl_del_and_deinit();
    return 1;
}
