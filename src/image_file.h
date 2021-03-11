/*
 * image_file.h
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

#pragma once

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <list>
#include <map>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#include "overlaybd/alog.h"
#include "overlaybd/async.h"
#include "overlaybd/fs/asyncfs.h"
#include "overlaybd/fs/exportfs.h"
#include "overlaybd/fs/filesystem.h"
#include "overlaybd/fs/forwardfs.h"
#include "overlaybd/fs/lsmt/file.h"
#include "overlaybd/photon/thread11.h"
#include "config.h"
#include "bk_download.h"
#include "get_image_file.h"

class ImageFile : public FileSystem::ForwardFile {
public:
    ImageFile(ImageConfigNS::ImageConfig &_conf, struct GlobalFs &_gfs,
              ImageConfigNS::GlobalConfig &_gconf)
        : gfs(_gfs), gconf(_gconf), ForwardFile(nullptr) {
        conf.CopyFrom(_conf, conf.GetAllocator());
        m_exception = "";
        m_status = init_image_file();
        if (m_file != NULL)
            m_afile = FileSystem::export_as_async_file(m_file);
    }

    ~ImageFile() {
        delete m_afile; // also destory m_file
    }

    int close() override {
        m_status = -1;
        if (dl_thread_jh != nullptr)
            photon::thread_join(dl_thread_jh);
        return m_file->close();
    }

    void async_pread(void *buf, size_t count, off_t offset, Callback<AsyncResult<ssize_t> *> cb) {
        m_afile->pread(buf, count, offset, cb);
    }

    void async_pwrite(void *buf, size_t count, off_t offset, Callback<AsyncResult<ssize_t> *> cb) {
        m_afile->pwrite(buf, count, offset, cb);
    }

    void async_sync(Callback<AsyncResult<int> *> cb) {
        if (m_read_only)
            return;
        m_afile->fdatasync(cb);
    }
    void async_unmap(off_t offset, size_t len, Callback<AsyncResult<int> *> cb) {
        m_afile->fallocate(3, offset, len, cb);
    }

    void set_auth_failed();
    int open_lower_layer(FileSystem::IFile *&file, ImageConfigNS::LayerConfig &layer, int index);

    std::string m_exception;
    int m_status = 0; // 0: not started, 1: running, -1 exit
    bool m_read_only = false;

private:
    FileSystem::IAsyncFile *m_afile = nullptr;
    struct GlobalFs &gfs;
    ImageConfigNS::GlobalConfig &gconf;
    ImageConfigNS::ImageConfig conf;
    std::list<BKDL::BkDownload *> dl_list;
    photon::join_handle *dl_thread_jh = nullptr;

    int init_image_file();
    void set_failed(std::string reason);
    LSMT::IFileRO *open_lowers(std::vector<ImageConfigNS::LayerConfig> &, bool &);
    LSMT::IFileRW *open_upper(ImageConfigNS::UpperConfig &);
    FileSystem::IFile *__open_ro_file(const std::string &);
    FileSystem::IFile *__open_ro_remote(const std::string &dir, const std::string &,
                                        const uint64_t);
    void start_bk_dl_thread();
};
