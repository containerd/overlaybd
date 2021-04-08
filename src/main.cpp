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
#include "image_file.h"
#include "image_service.h"
#include "overlaybd/alog.h"
#include "overlaybd/event-loop.h"
#include "overlaybd/fs/filesystem.h"
#include "overlaybd/net/curl.h"
#include "overlaybd/photon/syncio/aio-wrapper.h"
#include "overlaybd/photon/syncio/fd-events.h"
#include "overlaybd/photon/syncio/signal.h"
#include "overlaybd/photon/thread-pool.h"
#include "overlaybd/photon/thread.h"
#include "tcmu-runner/libtcmu.h"
#include "tcmu-runner/libtcmu_common.h"
#include "tcmu-runner/scsi.h"
#include "tcmu-runner/scsi_defs.h"
#include "scsi_helper.h"
#include <fcntl.h>
#include <scsi/scsi.h>
#include <sys/resource.h>

class TCMUDevLoop;

#define MAX_OPEN_FD 1048576

struct obd_dev {
    ImageFile *file;
    TCMUDevLoop *loop;
    uint32_t aio_pending_wakeups;
    uint32_t inflight;
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
    EventLoop *loop;
    struct tcmulib_context *ctx;
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
        : ctx(ctx), loop(new_event_loop({this, &TCMULoop::wait_for_readable},
                                        {this, &TCMULoop::on_accept})) {
        fd = tcmulib_get_master_fd(ctx);
    }

    ~TCMULoop() {
        loop->stop();
        delete loop;
    }

    void run() { loop->async_run(); }
};

void cmd_handler(struct tcmu_device *dev, struct tcmulib_cmd *cmd) {
    obd_dev *odev = (obd_dev *)tcmu_dev_get_private(dev);
    ImageFile *file = odev->file;
    size_t ret = -1;
    size_t length;

    switch (cmd->cdb[0]) {
    case INQUIRY:
        photon::thread_yield();
        ret =
            tcmu_emulate_inquiry(dev, NULL, cmd->cdb, cmd->iovec, cmd->iov_cnt);
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
            ret = tcmu_emulate_read_capacity_16(file->num_lbas,
                                                file->block_size, cmd->cdb,
                                                cmd->iovec, cmd->iov_cnt);
        else
            ret = TCMU_STS_NOT_HANDLED;
        tcmulib_command_complete(dev, cmd, ret);
        break;

    case MODE_SENSE:
    case MODE_SENSE_10:
        photon::thread_yield();
        ret = emulate_mode_sense(dev, cmd->cdb, cmd->iovec, cmd->iov_cnt, file->read_only);
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
        ret = file->preadv(cmd->iovec, cmd->iov_cnt,
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
        ret = file->pwritev(cmd->iovec, cmd->iov_cnt,
                            tcmu_cdb_to_byte(dev, cmd->cdb));
        if (ret == length) {
            tcmulib_command_complete(dev, cmd, TCMU_STS_OK);
        } else {
            if (errno == EROFS) {
                tcmulib_command_complete(dev, cmd,
                                         TCMU_STS_WR_ERR_INCOMPAT_FRMT);
            }
            tcmulib_command_complete(dev, cmd, TCMU_STS_WR_ERR);
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
            length = tcmu_iovec_length(cmd->iovec, cmd->iov_cnt);
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
}

class TCMUDevLoop {
protected:
    EventLoop *loop;
    struct tcmu_device *dev;
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

    void run() { loop->async_run(); }
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
    if (!config)
        return -EPERM;

    ImageFile *file = imgservice->create_image_file(config);
    if (file == nullptr) {
        return -EPERM;
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

    odev->loop = new TCMUDevLoop(dev);
    odev->loop->run();

    return 0;
}

static int close_cnt = 0;
static void dev_close(struct tcmu_device *dev) {
    obd_dev *odev = (obd_dev *)tcmu_dev_get_private(dev);
    delete odev->loop;
    odev->file->close();
    delete odev->file;
    delete odev;
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

    photon::init();
    DEFER(photon::fini());
    photon::fd_events_init();
    DEFER(photon::fd_events_fini());
    photon::libaio_wrapper_init();
    DEFER(photon::libaio_wrapper_fini());
    photon::sync_signal_init();
    DEFER(photon::sync_signal_fini());
    Net::libcurl_init();
    DEFER(Net::libcurl_fini());

    photon::sync_signal(SIGINT, &sigint_handler);

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
                      (long long int)rlim.rlim_cur,
                      (long long int)rlim.rlim_max);
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

    struct tcmulib_context *tcmulib_ctx;
    struct tcmulib_handler main_handler;
    main_handler.name = "Handler for overlaybd devices";
    main_handler.subtype = "overlaybd";
    main_handler.cfg_desc = "overlaybd bs";
    main_handler.check_config = nullptr;
    main_handler.added = dev_open;
    main_handler.removed = dev_close;

    tcmulib_ctx = tcmulib_initialize(&main_handler, 1);
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
        photon::thread_sleep(1);
    }

    tcmulib_close(tcmulib_ctx);
    return 0;
}