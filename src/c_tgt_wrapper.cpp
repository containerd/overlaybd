/*
 * c_tgt_wrapper.cpp
 *
 * Copyright (C) 2021 Alibaba Group.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */

#include <errno.h>
#include <sys/signal.h>
#include <boost/lockfree/spsc_queue.hpp>
#include "overlaybd/alog.h"
#include "overlaybd/executor/executor.h"
#include "overlaybd/fs/filesystem.h"
#include "overlaybd/net/curl.h"
#include "overlaybd/photon/syncio/aio-wrapper.h"
#include "get_image_file.h"
#include "image_file.h"
#include "c_tgt_wrapper.h"

typedef boost::lockfree::spsc_queue<struct scsi_cmd *, boost::lockfree::capacity<4096>> spsc;

class ExecutorExt : public Executor::HybridEaseExecutor {
public:
    template <typename T, typename R = decltype(std::declval<T>()())>
    R sync_perform(T &&func) {
        return perform(std::forward<T>(func));
    }

    template <typename T, typename R = decltype(std::declval<T>()())>
    R async_perform(T &&func) {
        return async_perform(std::forward<T>(func));
    }

    ExecutorExt() {
        sync_perform([&] {
            sigset_t set;
            sigfillset(&set);
            sigprocmask(SIG_BLOCK, &set, NULL);
            LOG_INFO("photon thread sigprocmask set");

            Net::libcurl_init();
            photon::libaio_wrapper_init();
            FileSystem::exportfs_init();
        });
    }
    ~ExecutorExt() {
        sync_perform([&] {
            FileSystem::exportfs_fini();
            photon::libaio_wrapper_fini();
            Net::libcurl_fini();
        });
    }

    static ExecutorExt &get() {
        static ExecutorExt instance;
        return instance;
    }
};

struct ImageFile *ex_perform_get_ifile(char *config_path, size_t *size, int *ro,
                                       uint32_t *blksize) {
    struct ImageFile *ret = ExecutorExt::get().sync_perform([&] {
        LOG_INFO("get_obd_ifile(), config_path:`", config_path);
        ImageFile *ret = get_image_file(config_path);

        if (ret == nullptr) {
            LOG_ERROR("get_image_file(...) return NULL");
            return (ImageFile *)nullptr;
        }

        struct stat stat_buf;
        ((FileSystem::IFile *)ret)->fstat(&stat_buf);
        *size = stat_buf.st_size;
        *ro = ret->m_read_only;
        *blksize = stat_buf.st_blksize;
        LOG_INFO("get_obd_ifile(), size:`, ro:`, blksize:`", *size, *ro, *blksize);

        return ret;
    });
    return ret;
}

int ex_perform_ifile_close(struct ImageFile *ifile) {
    return ExecutorExt::get().sync_perform([&] {
        LOG_INFO("enter obd_file_close(.)");
        auto p = (::FileSystem::IFile *)ifile;
        int ret = p->close();
        LOG_INFO("obd_file_close(.), ret:`", ret);
        return ret;
    });
}

int ex_perform_ifile_exit(struct ImageFile *ifile) {
    return ExecutorExt::get().sync_perform([&] {
        LOG_INFO("enter obd_file_exit(.)");
        auto p = (::FileSystem::IFile *)ifile;
        if (p != nullptr) {
            delete p;
        }
        return 0;
    });
}

struct AsyncContext {
    struct scsi_cmd *cmd;
    request_cb_t *func;

    AsyncContext(struct scsi_cmd *_cmd, request_cb_t *_func) {
        cmd = _cmd;
        func = _func;
    }

    template <typename Obj>
    int callback(AsyncResult<Obj> *ar) {
        func(cmd, (uint32_t)ar->result);
        delete this;
        return 0;
    }
};

void ex_async_read(struct ImageFile *fd, void *buf, size_t count, off_t offset,
                   struct scsi_cmd *cmd, request_cb_t *func) {
    AsyncContext *ctx = new AsyncContext(cmd, func);
    Callback<AsyncResult<ssize_t> *> cb;
    cb.bind(ctx, &AsyncContext::callback);

    fd->async_pread(buf, count, offset, cb);
}

void ex_async_write(struct ImageFile *fd, void *buf, size_t count, off_t offset,
                    struct scsi_cmd *cmd, request_cb_t *func) {
    AsyncContext *ctx = new AsyncContext(cmd, func);
    Callback<AsyncResult<ssize_t> *> cb;
    cb.bind(ctx, &AsyncContext::callback);

    fd->async_pwrite(buf, count, offset, cb);
}

void ex_async_sync(struct ImageFile *fd, struct scsi_cmd *cmd, request_cb_t *func) {
    AsyncContext *ctx = new AsyncContext(cmd, func);
    Callback<AsyncResult<int> *> cb;
    cb.bind(ctx, &AsyncContext::callback);

    fd->async_sync(cb);
}

void ex_async_unmap(struct ImageFile *fd, off_t offset, size_t len, struct scsi_cmd *cmd, request_cb_t *func) {
    AsyncContext *ctx = new AsyncContext(cmd, func);
    Callback<AsyncResult<int> *> cb;
    cb.bind(ctx, &AsyncContext::callback);

    fd->async_unmap(offset, len, cb);
}

void *init_finish_queue() {
    void *queue = (void *)(new spsc);
    return queue;
}

void push_finish_queue(void *queue, struct scsi_cmd *cmd) {
    while (!((spsc *)queue)->push(cmd))
        sched_yield();
}

struct scsi_cmd *pop_finish_queue(void *queue) {
    struct scsi_cmd *cmd;
    ((spsc *)queue)->pop(cmd);
    return cmd;
}

void delete_finish_queue(void *queue) {
    delete (spsc *)queue;
}
