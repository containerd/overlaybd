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
#include <photon/common/alog.h>
#include <photon/common/event-loop.h>
#include <photon/fs/filesystem.h>
#include <photon/net/curl.h>
#include <photon/io/fd-events.h>
#include <photon/io/signal.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread-pool.h>

#include <libtcmu.h>
#include <libtcmu_common.h>
#include <scsi.h>
#include <scsi_defs.h>
#include <fcntl.h>
#include <scsi/scsi.h>
#include <sys/resource.h>
#include <sys/prctl.h>

class TCMUDevLoop;

#define MAX_OPEN_FD 1048576

struct obd_dev {
    ImageFile *file;
    TCMUDevLoop *loop;
    uint32_t aio_pending_wakeups;
    uint32_t inflight;
    std::thread *work;
    photon::semaphore start, end;
};

struct handle_args {
    struct tcmu_device *dev;
    struct tcmulib_cmd *cmd;
};

class TCMULoop;
TCMULoop *main_loop = nullptr;
ImageService *imgservice = nullptr;

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

void cmd_handler(struct tcmu_device *dev, struct tcmulib_cmd *cmd) {
    obd_dev *odev = (obd_dev *)tcmu_dev_get_private(dev);
    ImageFile *file = odev->file;
    size_t ret = -1;
    size_t length;

    switch (cmd->cdb[0]) {
    case INQUIRY:
        photon::thread_yield();
        ret = tcmu_emulate_inquiry(dev, NULL, cmd->cdb, cmd->iovec, cmd->iov_cnt);
        tcmulib_command_complete(dev, cmd, ret);
        break;

    case TEST_UNIT_READY:
        photon::thread_yield();
        ret = tcmu_emulate_test_unit_ready(cmd->cdb, cmd->iovec, cmd->iov_cnt);
        tcmulib_command_complete(dev, cmd, ret);
        break;

    case SERVICE_ACTION_IN_16:
        photon::thread_yield();
        if (cmd->cdb[1] == READ_CAPACITY_16)
            ret = tcmu_emulate_read_capacity_16(file->num_lbas, file->block_size, cmd->cdb,
                                                cmd->iovec, cmd->iov_cnt);
        else
            ret = TCMU_STS_NOT_HANDLED;
        tcmulib_command_complete(dev, cmd, ret);
        break;

    case MODE_SENSE:
    case MODE_SENSE_10:
        photon::thread_yield();
        ret = tcmu_emulate_mode_sense(dev, cmd->cdb, cmd->iovec, cmd->iov_cnt);
        tcmulib_command_complete(dev, cmd, ret);
        break;

    case MODE_SELECT:
    case MODE_SELECT_10:
        photon::thread_yield();
        ret = tcmu_emulate_mode_select(dev, cmd->cdb, cmd->iovec, cmd->iov_cnt);
        tcmulib_command_complete(dev, cmd, ret);
        break;

    case READ_6:
    case READ_10:
    case READ_12:
    case READ_16:
        length = tcmu_iovec_length(cmd->iovec, cmd->iov_cnt);
        ret = sure({file, &ImageFile::preadv}, cmd->iovec, cmd->iov_cnt,
                   tcmu_cdb_to_byte(dev, cmd->cdb));
        if (ret == length) {
            tcmulib_command_complete(dev, cmd, TCMU_STS_OK);
        } else {
            tcmulib_command_complete(dev, cmd, TCMU_STS_RD_ERR);
        }
        break;

    case WRITE_6:
    case WRITE_10:
    case WRITE_12:
    case WRITE_16:
        length = tcmu_iovec_length(cmd->iovec, cmd->iov_cnt);
        ret = file->pwritev(cmd->iovec, cmd->iov_cnt, tcmu_cdb_to_byte(dev, cmd->cdb));
        if (ret == length) {
            tcmulib_command_complete(dev, cmd, TCMU_STS_OK);
        } else {
            if (errno == EROFS) {
                tcmulib_command_complete(dev, cmd, TCMU_STS_WR_ERR_INCOMPAT_FRMT);
            } else {
                tcmulib_command_complete(dev, cmd, TCMU_STS_WR_ERR);
            }
        }
        break;

    case SYNCHRONIZE_CACHE:
    case SYNCHRONIZE_CACHE_16:
        ret = file->fdatasync();
        if (ret == 0) {
            tcmulib_command_complete(dev, cmd, TCMU_STS_OK);
        } else {
            tcmulib_command_complete(dev, cmd, TCMU_STS_WR_ERR);
        }
        break;

    case WRITE_SAME:
    case WRITE_SAME_16:
        if (cmd->cdb[1] & 0x08) {
            length = tcmu_lba_to_byte(dev, tcmu_cdb_get_xfer_length(cmd->cdb));
            ret = file->fallocate(3, tcmu_cdb_to_byte(dev, cmd->cdb), length);
            if (ret == 0) {
                tcmulib_command_complete(dev, cmd, TCMU_STS_OK);
            } else {
                tcmulib_command_complete(dev, cmd, TCMU_STS_WR_ERR);
            }
        } else {
            LOG_ERROR("unknown write_same command `", cmd->cdb[0]);
            tcmulib_command_complete(dev, cmd, TCMU_STS_NOT_HANDLED);
        }
        break;

    case MAINTENANCE_IN:
    case MAINTENANCE_OUT:
        tcmulib_command_complete(dev, cmd, TCMU_STS_NOT_HANDLED);
        break;

    default:
        LOG_ERROR("unknown command `", cmd->cdb[0]);
        tcmulib_command_complete(dev, cmd, TCMU_STS_NOT_HANDLED);
        break;
    }

    // call tcmulib_processing_complete(dev) if needed
    ++odev->aio_pending_wakeups;
    int wake_up = (odev->aio_pending_wakeups == 1) ? 1 : 0;
    while (wake_up) {
        tcmulib_processing_complete(dev);
        photon::thread_yield();

        if (odev->aio_pending_wakeups > 1) {
            odev->aio_pending_wakeups = 1;
            wake_up = 1;
        } else {
            odev->aio_pending_wakeups = 0;
            wake_up = 0;
        }
    }

    odev->inflight--;
}

void *handle(void *args) {
    handle_args *obj = (handle_args *)args;
    cmd_handler(obj->dev, obj->cmd);
    delete obj;
    return nullptr;
}

class TCMUDevLoop {
protected:
    struct tcmu_device *dev;
    EventLoop *loop;
    int fd;
    photon::ThreadPool<32> threadpool;

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
        tcmulib_processing_start(dev);
        while ((cmd = tcmulib_get_next_command(dev, 0)) != NULL) {
            odev->inflight++;
            threadpool.thread_create(&handle, new handle_args{dev, cmd});
        }
        return 0;
    }

public:
    explicit TCMUDevLoop(struct tcmu_device *dev)
        : dev(dev), loop(new_event_loop({this, &TCMUDevLoop::wait_for_readable},
                                        {this, &TCMUDevLoop::on_accept})) {
        fd = tcmu_dev_get_fd(dev);
    }

    ~TCMUDevLoop() {
        loop->stop();
        delete loop;
    }

    void run() {
        loop->async_run();
    }
};

static char *tcmu_get_path(struct tcmu_device *dev) {
    char *config = strchr(tcmu_dev_get_cfgstring(dev), '/');
    if (!config) {
        LOG_ERROR("no configuration found in cfgstring");
        return NULL;
    }
    config += 1;

    return config;
}

static int dev_open(struct tcmu_device *dev) {
    char *config = tcmu_get_path(dev);
    LOG_INFO("dev open `", config);
    if (!config) {
        LOG_ERROR_RETURN(0, -EPERM, "get image config path failed");
    }

    struct timeval start;
    gettimeofday(&start, NULL);

    ImageFile *file = imgservice->create_image_file(config);
    if (file == nullptr) {
        LOG_ERROR_RETURN(0, -EPERM, "create image file failed");
    }

    obd_dev *odev = new obd_dev;
    odev->aio_pending_wakeups = 0;
    odev->inflight = 0;
    odev->file = file;

    tcmu_dev_set_private(dev, odev);
    tcmu_dev_set_block_size(dev, file->block_size);
    tcmu_dev_set_num_lbas(dev, file->num_lbas);
    tcmu_dev_set_unmap_enabled(dev, true);
    tcmu_dev_set_write_cache_enabled(dev, false);
    tcmu_dev_set_write_protect_enabled(dev, file->read_only);

    if (imgservice->global_conf.enableThread()) {
        auto obd_th = [](obd_dev *odev, struct tcmu_device *dev) {
            photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_LIBCURL);
            DEFER(photon::fini());

            odev->loop = new TCMUDevLoop(dev);
            odev->loop->run();
            LOG_INFO("obd device running");
            odev->start.signal(1);

            odev->end.wait(1);
            delete odev->loop;
            LOG_INFO("obd device exit");
        };

        odev->work = new std::thread(obd_th, odev, dev);
        odev->start.wait(1);
    } else {
        odev->loop = new TCMUDevLoop(dev);
        odev->loop->run();
    }

    struct timeval end;
    gettimeofday(&end, NULL);

    uint64_t elapsed = 1000000UL * (end.tv_sec - start.tv_sec) + end.tv_usec - start.tv_usec;
    LOG_INFO("dev opened `, time cost ` ms", config, elapsed / 1000);
    return 0;
}

static int close_cnt = 0;
static void dev_close(struct tcmu_device *dev) {
    obd_dev *odev = (obd_dev *)tcmu_dev_get_private(dev);
    if (imgservice->global_conf.enableThread()) {
        odev->end.signal(1);
        if (odev->work->joinable()) {
            odev->work->join();
        }
        delete odev->work;
    } else {
        delete odev->loop;
    }
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

    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
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

    delete imgservice;
    return 0;
}
