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
#include "version.h"
#include "image_file.h"
#include "image_service.h"
#include "tools/comm_func.h"
#include "libtcmu/libtcmu.h"
#include "libtcmu/libtcmu_common.h"

#include <photon/common/alog.h>
#include <photon/common/event-loop.h>
#include <photon/fs/filesystem.h>
#include <photon/net/curl.h>
#include <photon/io/fd-events.h>
#include <photon/io/signal.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>

#include <scsi.h>
#include <scsi_defs.h>
#include <fcntl.h>
#include <scsi/scsi.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <linux/netlink.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

class TCMUDevLoop;
class TCMUWorkPool;

#define MAX_OPEN_FD 1048576

struct obd_dev {
    ImageFile *file;
    TCMUDevLoop *loop;
    std::atomic<uint64_t> inflight{0};
    std::atomic<struct tcmulib_cmd *> completed{nullptr};
    photon::semaphore completion_sem{0};
    std::atomic<bool> completion_stopping{false};
    std::string dev_id;
};

class TCMULoop;
TCMULoop *main_loop = nullptr;
ImageService *imgservice = nullptr;
TCMUWorkPool *io_work_pool = nullptr;

class TCMULoop {
protected:
    struct tcmulib_context *ctx;
    EventLoop *loop;
    int fd;

    int wait_for_readable(EventLoop *) {
        auto ret = photon::wait_for_fd_readable(fd);
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                return 0;
            }
            return -1;
        }
        return 1;
    }

    int on_accept(EventLoop *) {
        tcmulib_master_fd_ready(ctx);
        return 0;
    }

public:
    explicit TCMULoop(struct tcmulib_context *ctx)
        : ctx(ctx),
          loop(new_event_loop({this, &TCMULoop::wait_for_readable}, {this, &TCMULoop::on_accept})) {
        fd = tcmulib_get_master_fd(ctx);

        // libnl3 defaults the socket receive buffer to 32KB which can be 
        // overrun during concurrent device creation. This causes
        // TCMU_CMD_ADDED_DEVICE messages to be silently dropped and leaves 
        // kernel threads stuck in tcmu_wait_genl_cmd_reply().
        // SO_RCVBUFFORCE requires CAP_NET_ADMIN (the daemon runs as root).
        static constexpr int NETLINK_RCVBUF_SIZE = 4 * 1024 * 1024;
        int rcvbuf = NETLINK_RCVBUF_SIZE;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE,
                        &rcvbuf, sizeof(rcvbuf)) < 0) {
            LOG_ERROR("setsockopt(SO_RCVBUFFORCE) failed: `. "
                      "Netlink buffer remains at libnl3 default (32KB); "
                      "concurrent device creation may cause kernel hangs.",
                      strerror(errno));
        }

        // NETLINK_NO_ENOBUFS prevents netlink_overrun() from setting
        // the NETLINK_S_CONGESTED sticky flag on the socket. Without this,
        // a single buffer-full event poisons all future deliveries until
        // the buffer fully drains, cascading the original drop into a
        // complete stall.
        int no_enobufs = 1;
        if (setsockopt(fd, SOL_NETLINK, NETLINK_NO_ENOBUFS,
                        &no_enobufs, sizeof(no_enobufs)) < 0) {
            LOG_WARN("setsockopt(NETLINK_NO_ENOBUFS) failed: `", strerror(errno));
        }
    }

    ~TCMULoop() {
        loop->stop();
        delete loop;
    }

    void run() {
        loop->async_run();
    }
};

using SureIODelegate = Delegate<ssize_t, const struct iovec *, int, off_t>;

ssize_t sure(SureIODelegate io, const struct iovec *iov, int iovcnt, off_t offset) {
    auto time_st = photon::now;
    uint64_t try_cnt = 0, sleep_period = 20UL * 1000;
again:
    if (photon::now - time_st > 7LL * 24 * 60 * 60 * 1000 * 1000 /*7days*/) {
        LOG_ERROR_RETURN(EIO, -1, "sure request timeout, offset: `", offset);
    }
    ssize_t ret = io(iov, iovcnt, offset);
    if (ret >= 0) {
        return ret;
    }
    if (try_cnt % 10 == 0) {
        LOG_ERROR("io request failed, offset: `, ret: `, retry times: `, errno:`", offset, ret,
                  try_cnt, errno);
    }
    try_cnt++;
    photon::thread_usleep(sleep_period);
    sleep_period = std::min(sleep_period * 2, 30UL * 1000 * 1000);
    goto again;
}

void complete_command(struct tcmu_device *dev, struct tcmulib_cmd *cmd,
                      int result) {
    obd_dev *odev = (obd_dev *)tcmu_dev_get_private(dev);
    cmd->completion_result = result;

    auto head = odev->completed.load(std::memory_order_relaxed);
    do {
        cmd->completion_next = head;
    } while (!odev->completed.compare_exchange_weak(
        head, cmd, std::memory_order_release, std::memory_order_relaxed));

    odev->completion_sem.signal(1);
}

void cmd_handler(struct tcmu_device *dev, struct tcmulib_cmd *cmd) {
    obd_dev *odev = (obd_dev *)tcmu_dev_get_private(dev);
    ImageFile *file = odev->file;
    size_t ret = -1;
    size_t length;

    switch (cmd->cdb[0]) {
    case INQUIRY:
        photon::thread_yield();
        ret = tcmu_emulate_inquiry(dev, NULL, cmd->cdb, cmd->iovec, cmd->iov_cnt);
        complete_command(dev, cmd, ret);
        break;

    case TEST_UNIT_READY:
        photon::thread_yield();
        ret = tcmu_emulate_test_unit_ready(cmd->cdb, cmd->iovec, cmd->iov_cnt);
        complete_command(dev, cmd, ret);
        break;

    case SERVICE_ACTION_IN_16:
        photon::thread_yield();
        if (cmd->cdb[1] == READ_CAPACITY_16)
            ret = tcmu_emulate_read_capacity_16(file->num_lbas, file->block_size, cmd->cdb,
                                                cmd->iovec, cmd->iov_cnt);
        else
            ret = TCMU_STS_NOT_HANDLED;
        complete_command(dev, cmd, ret);
        break;

    case MODE_SENSE:
    case MODE_SENSE_10:
        photon::thread_yield();
        ret = tcmu_emulate_mode_sense(dev, cmd->cdb, cmd->iovec, cmd->iov_cnt);
        complete_command(dev, cmd, ret);
        break;

    case MODE_SELECT:
    case MODE_SELECT_10:
        photon::thread_yield();
        ret = tcmu_emulate_mode_select(dev, cmd->cdb, cmd->iovec, cmd->iov_cnt);
        complete_command(dev, cmd, ret);
        break;

    case READ_6:
    case READ_10:
    case READ_12:
    case READ_16:
        length = tcmu_iovec_length(cmd->iovec, cmd->iov_cnt);
        ret = sure({file, &ImageFile::preadv}, cmd->iovec, cmd->iov_cnt,
                   tcmu_cdb_to_byte(dev, cmd->cdb));
        if (ret == length) {
            complete_command(dev, cmd, TCMU_STS_OK);
        } else {
            complete_command(dev, cmd, TCMU_STS_RD_ERR);
        }
        break;

    case WRITE_6:
    case WRITE_10:
    case WRITE_12:
    case WRITE_16:
        length = tcmu_iovec_length(cmd->iovec, cmd->iov_cnt);
        ret = file->pwritev(cmd->iovec, cmd->iov_cnt, tcmu_cdb_to_byte(dev, cmd->cdb));
        if (ret == length) {
            complete_command(dev, cmd, TCMU_STS_OK);
        } else {
            if (errno == EROFS) {
                complete_command(dev, cmd, TCMU_STS_WR_ERR_INCOMPAT_FRMT);
            } else {
                complete_command(dev, cmd, TCMU_STS_WR_ERR);
            }
        }
        break;

    case SYNCHRONIZE_CACHE:
    case SYNCHRONIZE_CACHE_16:
        ret = file->fdatasync();
        if (ret == 0) {
            complete_command(dev, cmd, TCMU_STS_OK);
        } else {
            complete_command(dev, cmd, TCMU_STS_WR_ERR);
        }
        break;

    case WRITE_SAME:
    case WRITE_SAME_16:
        if (cmd->cdb[1] & 0x08) {
            length = tcmu_lba_to_byte(dev, tcmu_cdb_get_xfer_length(cmd->cdb));
            ret = file->fallocate(3, tcmu_cdb_to_byte(dev, cmd->cdb), length);
            if (ret == 0) {
                complete_command(dev, cmd, TCMU_STS_OK);
            } else {
                complete_command(dev, cmd, TCMU_STS_WR_ERR);
            }
        } else {
            LOG_ERROR("unknown write_same command `", cmd->cdb[0]);
            complete_command(dev, cmd, TCMU_STS_NOT_HANDLED);
        }
        break;

    case MAINTENANCE_IN:
    case MAINTENANCE_OUT:
        complete_command(dev, cmd, TCMU_STS_NOT_HANDLED);
        break;

    default:
        LOG_ERROR("unknown command `", cmd->cdb[0]);
        complete_command(dev, cmd, TCMU_STS_NOT_HANDLED);
        break;
    }
}

class TCMUWorkPool : public photon::WorkPool {
public:
    TCMUWorkPool(size_t vcpu_num, int event_engine, int io_engine)
        : photon::WorkPool(vcpu_num, event_engine, io_engine, -1) {}

    void dispatch_commands(struct tcmu_device *dev,
                           const std::vector<struct tcmulib_cmd *> &commands,
                           bool device_idle) {
        if (commands.empty())
            return;

        if (device_idle && commands.size() == 1) {
            start_command(dev, commands.front());
            return;
        }

        const auto worker_count = std::min(
            commands.size(), static_cast<size_t>(get_vcpu_num()));
        for (size_t worker = 0; worker < worker_count; ++worker) {
            // Let WorkPool choose the next VCPU with its global round-robin.
            if (thread_migrate() != 0)
                LOG_ERROR("failed to migrate TCMU dispatcher into WorkPool");
            for (size_t i = worker; i < commands.size(); i += worker_count)
                start_command(dev, commands[i]);
        }

        if (migrate_to_home(photon::CURRENT, dev) != 0)
            LOG_ERROR("failed to return TCMU dispatcher to its home VCPU");
    }

    int migrate_to_home(photon::thread *thread, struct tcmu_device *dev) {
        return thread_migrate(thread, home_worker(dev));
    }

private:
    static void start_command(struct tcmu_device *dev,
                              struct tcmulib_cmd *cmd) {
        if (!photon::thread_create11(&cmd_handler, dev, cmd)) {
            LOG_ERROR("failed to create Photon thread for TCMU command");
            cmd_handler(dev, cmd);
        }
    }

    size_t home_worker(struct tcmu_device *dev) {
        return (reinterpret_cast<uintptr_t>(dev) >> 6) % get_vcpu_num();
    }
};

class TCMUDevLoop {
protected:
    struct tcmu_device *dev;
    EventLoop *loop;
    photon::join_handle *completion_thread;
    std::vector<struct tcmulib_cmd *> pending_commands;
    int fd;

    int wait_for_readable(EventLoop *) {
        auto ret = photon::wait_for_fd_readable(fd);
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                return 0;
            }
            return -1;
        }
        return 1;
    }

    int on_accept(EventLoop *) {
        struct tcmulib_cmd *cmd;
        obd_dev *odev = (obd_dev *)tcmu_dev_get_private(dev);
        pending_commands.clear();
        bool device_idle = false;
        tcmulib_processing_start(dev);
        while ((cmd = tcmulib_get_next_command(dev, 0)) != NULL) {
            auto prev = odev->inflight.fetch_add(1, std::memory_order_relaxed);
            if (pending_commands.empty())
                device_idle = (prev == 0);
            pending_commands.push_back(cmd);
        }
        io_work_pool->dispatch_commands(dev, pending_commands, device_idle);
        return 0;
    }

    void drain_completions() {
        obd_dev *odev = (obd_dev *)tcmu_dev_get_private(dev);
        auto commands = odev->completed.exchange(nullptr, std::memory_order_acquire);
        if (!commands)
            return;

        // The MPSC stack is LIFO. Reverse it to preserve the order in which
        // workers published completions before updating the response ring.
        struct tcmulib_cmd *ordered = nullptr;
        while (commands) {
            auto next = commands->completion_next;
            commands->completion_next = ordered;
            ordered = commands;
            commands = next;
        }

        uint32_t drained = 0;
        while (ordered) {
            auto next = ordered->completion_next;
            tcmulib_command_complete(dev, ordered, ordered->completion_result);
            ordered = next;
            drained++;
        }
        tcmulib_processing_complete(dev);
        odev->inflight.fetch_sub(drained, std::memory_order_release);
    }

    void run_completion_loop() {
        obd_dev *odev = (obd_dev *)tcmu_dev_get_private(dev);
        for (;;) {
            odev->completion_sem.wait(1);

            auto extra = odev->completion_sem.count();
            if (extra)
                odev->completion_sem.wait(extra);
            drain_completions();

            if (odev->completion_stopping.load(std::memory_order_acquire) &&
                odev->inflight.load(std::memory_order_acquire) == 0)
                return;
        }
    }

public:
    explicit TCMUDevLoop(struct tcmu_device *dev)
        : dev(dev), loop(new_event_loop({this, &TCMUDevLoop::wait_for_readable},
                                        {this, &TCMUDevLoop::on_accept})),
          completion_thread(nullptr) {
        fd = tcmu_dev_get_fd(dev);
    }

    ~TCMUDevLoop() {
        loop->stop();
        delete loop;
        obd_dev *odev = (obd_dev *)tcmu_dev_get_private(dev);
        while (odev->inflight.load(std::memory_order_acquire) != 0) {
            photon::thread_usleep(1000);
        }
        odev->completion_stopping.store(true, std::memory_order_release);
        odev->completion_sem.signal(1);
        photon::thread_join(completion_thread);
    }

    void run() {
        auto thread = photon::thread_create11(&TCMUDevLoop::run_completion_loop, this);
        if (!thread) {
            LOG_ERROR("failed to create TCMU completion thread");
            abort();
        }
        completion_thread = photon::thread_enable_join(thread);
        if (io_work_pool->migrate_to_home(thread, dev) != 0) {
            LOG_ERROR("failed to migrate TCMU completion thread to WorkPool");
            abort();
        }
        loop->async_run();
        if (io_work_pool->migrate_to_home(loop->loop_thread(), dev) != 0) {
            LOG_ERROR("failed to migrate TCMU accept loop to WorkPool");
            abort();
        }
    }
};

static char *tcmu_get_path(struct tcmu_device *dev) {
    char *config = strchr(tcmu_dev_get_cfgstring(dev), '/'); // dev_config=overlaybd/<config_path>[;<dev_id>]
    if (!config) {
        LOG_ERROR("no configuration found in cfgstring");
        return NULL;
    }
    config += 1;

    return config;
}

static int dev_open(struct tcmu_device *dev) {
    char *config = tcmu_get_path(dev); // <config_path>[;<dev_id>]
    LOG_INFO("dev open `", config);
    if (!config) {
        LOG_ERROR_RETURN(0, -EPERM, "get image config path failed");
    }
    std::string config_path, dev_id;
    parse_config_and_dev_id(config, config_path, dev_id);

    struct timeval start;
    gettimeofday(&start, NULL);

    ImageFile *file = imgservice->create_image_file(config_path.c_str(), dev_id);
    if (file == nullptr) {
        LOG_ERROR_RETURN(0, -EPERM, "create image file failed");
    }

    obd_dev *odev = new obd_dev;
    odev->file = file;
    odev->dev_id = dev_id;

    tcmu_dev_set_private(dev, odev);
    tcmu_dev_set_block_size(dev, file->block_size);
    tcmu_dev_set_num_lbas(dev, file->num_lbas);
    tcmu_dev_set_unmap_enabled(dev, true);
    tcmu_dev_set_write_cache_enabled(dev, false);
    tcmu_dev_set_write_protect_enabled(dev, file->read_only);

    odev->loop = new TCMUDevLoop(dev);
    odev->loop->run();
    LOG_INFO("obd device running");

    struct timeval end;
    gettimeofday(&end, NULL);

    uint64_t elapsed = 1000000UL * (end.tv_sec - start.tv_sec) + end.tv_usec - start.tv_usec;
    LOG_INFO("dev opened `, time cost ` ms", config_path.c_str(), elapsed / 1000);
    return 0;
}

static int close_cnt = 0;
static void dev_close(struct tcmu_device *dev) {
    obd_dev *odev = (obd_dev *)tcmu_dev_get_private(dev);
    delete odev->loop;
    delete odev->file;
    delete odev;
    LOG_INFO("dev closed `", tcmu_get_path(dev));
    close_cnt++;
    if (close_cnt == 500) {
        malloc_trim(128 * 1024);
        close_cnt = 0;
    }
    return;
}

void sigint_handler(int signal = SIGINT) {
    LOG_INFO("sigint received");
    if (main_loop != nullptr) {
        delete main_loop;
        main_loop = nullptr;
    }
}

int main(int argc, char **argv) {
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    prctl(PR_SET_THP_DISABLE, 1);

    photon::PhotonOptions photon_options;
    photon_options.use_pooled_stack_allocator = true;
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT, photon_options);
    photon::block_all_signal();
    photon::sync_signal(SIGTERM, &sigint_handler);
    photon::sync_signal(SIGINT, &sigint_handler);
    if (argc > 1)
        imgservice = create_image_service(argv[1]);
    else
        imgservice = create_image_service();
    if (imgservice == nullptr) {
        LOG_ERROR("failed to create image service");
        return -1;
    }

    std::unique_ptr<TCMUWorkPool> work_pool;
    auto pool_size = imgservice->global_conf.workpoolSize();
    if (pool_size == 0) {
        LOG_WARN("workpoolSize is zero, using one worker");
        pool_size = 1;
    }
    work_pool.reset(new TCMUWorkPool(pool_size, photon::INIT_EVENT_EPOLL,
                                    photon::INIT_IO_LIBCURL));
    io_work_pool = work_pool.get();
    LOG_INFO("I/O work pool initialized with ` workers", pool_size);

    /*
     * Handings for rlimit and netlink are from tcmu-runner main.c
     */
    struct rlimit rlim;
    int ret = getrlimit(RLIMIT_NOFILE, &rlim);
    if (ret == -1) {
        LOG_ERROR("failed to get max open fd limit");
        return ret;
    }
    if (rlim.rlim_max < MAX_OPEN_FD) {
        rlim.rlim_max = MAX_OPEN_FD;
        ret = setrlimit(RLIMIT_NOFILE, &rlim);
        if (ret == -1) {
            LOG_ERROR("failed to set max open fd to [soft: ` hard: `]",
                      (long long int)rlim.rlim_cur, (long long int)rlim.rlim_max);
            return ret;
        }
    }

    /*
     * If this is a restart we need to prevent new nl cmds from being
     * sent to us until we have everything ready.
     */
    LOG_INFO("blocking netlink");
    bool reset_nl_supp = true;
    ret = tcmu_cfgfs_mod_param_set_u32("block_netlink", 1);
    LOG_INFO("blocking netlink done");
    if (ret == -ENOENT) {
        reset_nl_supp = false;
    } else {
        /*
         * If it exists ignore errors and try to reset in case kernel is
         * in an invalid state
         */
        LOG_INFO("resetting netlink");
        tcmu_cfgfs_mod_param_set_u32("reset_netlink", 1);
        LOG_INFO("reset netlink done");
    }

    LOG_INFO("current version: `", OVERLAYBD_VERSION);

    std::vector<struct tcmulib_handler> handlers;
    struct tcmulib_handler overlaybd_handler;
    overlaybd_handler.name = "Handler for overlaybd devices";
    overlaybd_handler.subtype = "overlaybd";
    overlaybd_handler.cfg_desc = "overlaybd bs";
    overlaybd_handler.check_config = nullptr;
    overlaybd_handler.added = dev_open;
    overlaybd_handler.removed = dev_close;
    handlers.push_back(overlaybd_handler);

    struct tcmulib_context *tcmulib_ctx = tcmulib_initialize(handlers);
    if (!tcmulib_ctx) {
        LOG_ERROR("tcmulib init failed.");
        return -1;
    }

    if (reset_nl_supp) {
        tcmu_cfgfs_mod_param_set_u32("block_netlink", 0);
        reset_nl_supp = false;
    }

    main_loop = new TCMULoop(tcmulib_ctx);
    main_loop->run();

    while (main_loop != nullptr) {
        photon::thread_usleep(200 * 1000);
    }
    LOG_INFO("main loop exited");

    tcmulib_close(tcmulib_ctx);
    LOG_INFO("tcmulib closed");

    io_work_pool = nullptr;
    work_pool.reset();
    delete imgservice;
    return 0;
}
