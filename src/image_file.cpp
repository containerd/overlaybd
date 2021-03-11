/*
 * image_file.cpp
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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

#include "overlaybd/alog-stdstring.h"
#include "overlaybd/alog.h"
#include "overlaybd/fs/aligned-file.h"
#include "overlaybd/fs/filesystem.h"
#include "overlaybd/fs/localfs.h"
#include "overlaybd/fs/lsmt/file.h"
#include "overlaybd/fs/zfile/zfile.h"
#include "config.h"
#include "image_file.h"
#include "sure_file.h"
#include "switch_file.h"

#define PARALLEL_LOAD_INDEX 8

FileSystem::IFile *ImageFile::__open_ro_file(const std::string &path) {
    int flags = O_RDONLY;

    LOG_INFO("open ro file: `", path);
    int ioengine = gconf.ioEngine();
    if (ioengine > 2) {
        LOG_WARN("invalid ioengine: `, set to psync", ioengine);
        ioengine = 0;
    }
    if (ioengine == IOEngineType::io_engine_libaio) {
        flags |= O_DIRECT;
        LOG_INFO("`: flag add O_DIRECT", path);
    }

    auto file = FileSystem::open_localfile_adaptor(path.c_str(), flags, 0644, ioengine);
    if (!file) {
        set_failed("failed to open local file " + path);
        LOG_ERROR_RETURN(0, nullptr, "open(`),`:`", path, errno, strerror(errno));
    }

    if (flags & O_DIRECT) {
        LOG_INFO("create aligned file. IO_FLAGS: `", flags);
        auto aligned_file = new_aligned_file_adaptor(file, FileSystem::ALIGNMENT_4K, true, true);
        if (!aligned_file) {
            set_failed("failed to open aligned_file_adaptor " + path);
            delete file;
            LOG_ERROR_RETURN(0, nullptr, "new_aligned_file_adaptor(`) failed, `:`", path, errno,
                             strerror(errno));
        }
        file = aligned_file;
    }

    if (ZFile::is_zfile(file) == 1) {
        auto zf = ZFile::zfile_open_ro(file, false, true);
        if (!zf) {
            set_failed("failed to open zfile " + path);
            delete file;
            LOG_ERROR_RETURN(0, nullptr, "zfile_open_ro(`) failed, `:`", path, errno,
                             strerror(errno));
        }
        file = zf;
    }

    return file;
}

FileSystem::IFile *ImageFile::__open_ro_remote(const std::string &dir, const std::string &digest,
                                               const uint64_t size) {
    std::string url;
    int64_t extra_range, rand_wait;

    if (conf.repoBlobUrl() == "") {
        set_failed("empty repoBlobUrl");
        LOG_ERROR_RETURN(0, nullptr, "empty repoBlobUrl for remote layer");
    }
    url = conf.repoBlobUrl();

    if (url[url.length() - 1] != '/')
        url += "/";
    url += digest;

    LOG_INFO("open file from remotefs: `, size: `", url, size);
    FileSystem::IFile *remote_file = gfs.remote_fs->open(url.c_str(), O_RDONLY);
    if (!remote_file) {
        if (errno == EPERM)
            set_auth_failed();
        else
            set_failed("failed to open remote file " + url);
        LOG_ERROR_RETURN(0, nullptr, "failed to open remote file `", url);
    }
    FileSystem::ISwitchFile *switch_file = FileSystem::new_switch_file(remote_file);
    if (!switch_file) {
        set_failed("failed to open switch file `" + url);
        delete remote_file;
        LOG_ERROR_RETURN(0, nullptr, "failed to open switch file `", url);
    }
    FileSystem::IFile *sure_file = new_sure_file(switch_file, this);
    if (!sure_file) {
        set_failed("failed to open sure file `" + url);
        delete switch_file;
        LOG_ERROR_RETURN(0, nullptr, "failed to open sure file `", url);
    }

    if (conf.HasMember("download") && conf.download().enable() == 1) {
        BKDL::BkDownload *obj =
            new BKDL::BkDownload(switch_file, remote_file, dir, conf.download().maxMBps(),
                                 conf.download().tryCnt(), this);
        LOG_DEBUG("add to download list for `", dir);
        dl_list.push_back(obj);
    }

    return sure_file;
}

void ImageFile::start_bk_dl_thread() {
    if (dl_list.empty()) {
        LOG_INFO("no need to download");
        return;
    }

    uint64_t extra_range = conf.download().delayExtra();
    extra_range = (extra_range <= 0) ? 30 : extra_range;
    uint64_t delay_sec = (rand() % extra_range) + conf.download().delay();

    dl_thread_jh = photon::thread_enable_join(
        photon::thread_create11(&BKDL::bk_download_proc, dl_list, delay_sec, m_status));
}

struct ParallelOpenTask {
    std::vector<FileSystem::IFile *> &files;
    int eno = 0;
    std::vector<ImageConfigNS::LayerConfig> &layers;
    int i = 0, nlayers;

    int get_next_job_index() {
        LOG_DEBUG("create job, layer_id: `", i);
        if (i < nlayers) {
            int res = i;
            i++;
            return res;
        }
        return -1;
    }
    void set_error(int eno) {
        this->eno = eno;
    }

    ParallelOpenTask(std::vector<FileSystem::IFile *> &files, size_t nlayers,
                     std::vector<ImageConfigNS::LayerConfig> &layers)
        : files(files), nlayers(nlayers), layers(layers) {
    }
};

void *do_parallel_open_files(ImageFile *imgfile, ParallelOpenTask &tm) {
    while (true) {
        int idx = tm.get_next_job_index();
        if (idx == -1 || tm.eno != 0) {
            // error occured from another threads.
            return nullptr;
        }
        int ret = imgfile->open_lower_layer(tm.files[idx], tm.layers[idx], idx);
        if (ret < 0) {
            tm.set_error(errno);
            LOG_ERROR_RETURN(0, nullptr, "failed to open files");
        }
    }
    return nullptr;
}

int ImageFile::open_lower_layer(FileSystem::IFile *&file, ImageConfigNS::LayerConfig &layer,
                                int index) {
    std::string opened;
    if (layer.file() != "") {
        opened = layer.file();
        file = __open_ro_file(opened);
    } else {
        // open downloaded blob or remote blob
        if (BKDL::check_downloaded(layer.dir())) {
            opened = layer.dir() + "/" + BKDL::COMMIT_FILE_NAME;
            file = __open_ro_file(opened);
        } else {
            opened = layer.digest();
            file = __open_ro_remote(layer.dir(), layer.digest(), layer.size());
        }
    }
    if (file != nullptr) {
        LOG_INFO("layer index: `, open(`) success", index, opened);
        return 0;
    }
    return -1;
}

LSMT::IFileRO *ImageFile::open_lowers(std::vector<ImageConfigNS::LayerConfig> &lowers,
                                      bool &has_error) {
    LSMT::IFileRO *ret = NULL;
    has_error = false;

    if (lowers.size() == 0)
        return NULL;

    photon::join_handle *ths[PARALLEL_LOAD_INDEX];
    std::vector<FileSystem::IFile *> files;
    files.resize(lowers.size(), nullptr);
    auto n = std::min(PARALLEL_LOAD_INDEX, (int)lowers.size());
    LOG_INFO("create ` photon threads to open lowers", n);

    ParallelOpenTask tm(files, lowers.size(), lowers);
    for (auto i = 0; i < n; ++i) {
        ths[i] =
            photon::thread_enable_join(photon::thread_create11(&do_parallel_open_files, this, tm));
    }

    for (int i = 0; i < n; i++) {
        photon::thread_join(ths[i]);
    }

    for (int i = 0; i < files.size(); i++) {
        if (files[i] == NULL) {
            LOG_ERROR("layer index ` open failed, exit.", i);
            if (m_exception == "")
                m_exception = "failed to open layer " + std::to_string(i);

            goto ERROR_EXIT;
        }
    }
    ret = LSMT::open_files_ro((FileSystem::IFile **)&(files[0]), lowers.size(), true);
    if (!ret) {
        LOG_ERROR("LSMT::open_files_ro(files, `, `) return NULL", lowers.size(), true);
        goto ERROR_EXIT;
    }
    LOG_INFO("LSMT::open_files_ro(files, `) success", lowers.size());

    return ret;

ERROR_EXIT:
    if (m_exception == "") {
        m_exception = "failed to create overlaybd device";
    }
    for (int i = 0; i < lowers.size(); i++) {
        if (files[i] != NULL)
            delete files[i];
    }
    has_error = true;
    return NULL;
}

LSMT::IFileRW *ImageFile::open_upper(ImageConfigNS::UpperConfig &upper) {
    FileSystem::IFile *data_file = NULL;
    FileSystem::IFile *idx_file = NULL;
    LSMT::IFileRW *ret = NULL;

    LOG_INFO("upper layer : ` , `", upper.index(), upper.data());

    int dafa_file_flags = O_RDWR;

    data_file = new_sure_file_by_path(upper.data().c_str(), O_RDWR, this);
    if (!data_file) {
        LOG_ERROR("open(`,flags), `:`", upper.data(), errno, strerror(errno));
        goto ERROR_EXIT;
    }

    idx_file = new_sure_file_by_path(upper.index().c_str(), O_RDWR, this);
    if (!idx_file) {
        LOG_ERROR("open(`,flags), `:`", upper.index(), errno, strerror(errno));
        goto ERROR_EXIT;
    }

    ret = LSMT::open_file_rw(data_file, idx_file, true);
    if (!ret) {
        LOG_ERROR("LSMT::open_file_rw(`,`,`) return NULL", (uint64_t)data_file, (uint64_t)idx_file,
                  true);
        goto ERROR_EXIT;
    }

    return ret;

ERROR_EXIT:
    delete data_file;
    delete idx_file;
    delete ret;
    return NULL;
}

int ImageFile::init_image_file() {
    LSMT::IFileRO *lower_file = NULL;
    LSMT::IFileRW *upper_file = NULL;
    LSMT::IFileRW *stack_ret = NULL;

    bool has_error = false;
    auto lowers = conf.lowers();

    ImageConfigNS::UpperConfig upper;
    upper.CopyFrom(conf.upper(), upper.GetAllocator());
    lower_file = open_lowers(lowers, has_error);

    if (has_error) {
        // NOTE: lower_file is allowed to be NULL. In this case, there is only one layer.
        LOG_ERROR("open lower layer failed.");
        goto ERROR_EXIT;
    }

    if (upper.index() == "" || upper.data() == "") {
        LOG_WARN("RW layer's path not set. return RO layers.");
        m_file = lower_file;
        m_read_only = true;
        goto SUCCESS_EXIT;
    }

    upper_file = open_upper(upper);
    if (!upper_file) {
        LOG_ERROR("open upper layer failed.");
        goto ERROR_EXIT;
    }
    stack_ret = LSMT::stack_files(upper_file, lower_file, true, false);
    if (!stack_ret) {
        LOG_ERROR("LSMT::stack_files(`, `)", (uint64_t)upper_file, true);
        goto ERROR_EXIT;
    }
    m_file = stack_ret;
    m_read_only = false;

SUCCESS_EXIT:
    if (conf.download().enable() == true) {
        start_bk_dl_thread();
    }
    return 1;

ERROR_EXIT:
    if (lower_file)
        delete lower_file;
    if (upper_file)
        delete upper_file;
    return -1;
}

void ImageFile::set_auth_failed() {
    if (m_status == 0) // only set exit in image boot phase
    {
        m_status = -1;
        m_exception = "Authentication failed";
    }
}

void ImageFile::set_failed(std::string reason) {
    if (m_status == 0) // only set exit in image boot phase
    {
        m_status = -1;
        m_exception = reason;
    }
}