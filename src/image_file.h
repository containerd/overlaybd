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
#include <sys/stat.h>

#include "bk_download.h"
#include "config.h"
#include "image_service.h"
#include "prefetch.h"
#include "overlaybd/alog.h"
#include "overlaybd/fs/filesystem.h"
#include "overlaybd/fs/forwardfs.h"
#include "overlaybd/fs/lsmt/file.h"
#include "overlaybd/photon/thread11.h"

class ImageFile : public FileSystem::ForwardFile {
public:
    ImageFile(ImageConfigNS::ImageConfig &_conf, struct GlobalFs &_gfs,
              ImageConfigNS::GlobalConfig &_gconf)
        : gfs(_gfs), gconf(_gconf), ForwardFile(nullptr) {
        conf.CopyFrom(_conf, conf.GetAllocator());
        m_exception = "";
        m_status = init_image_file();
        if (m_status == 1) {
            struct stat st;
            fstat(&st);
            LOG_INFO("new imageFile, bs: `, size: `", block_size, size);
        }
    }

    ~ImageFile() {
        delete m_file;
        delete m_prefetcher;
    }

    int close() override {
        m_status = -1;
        if (dl_thread_jh != nullptr)
            photon::thread_join(dl_thread_jh);
        return m_file->close();
    }

    int fstat(struct stat *buf) override {
        int ret = m_file->fstat(buf);
        block_size = buf->st_blksize;
        size = buf->st_size;
        if (block_size == 0)
            block_size = 512;
        num_lbas = size / block_size;
        return ret;
    }

    ssize_t pwritev(const struct iovec *iov, int iovcnt,
                    off_t offset) override {
        if (read_only) {
            LOG_ERROR_RETURN(EROFS, -1, "writing read only file");
        }
        return m_file->pwritev(iov, iovcnt, offset);
    }

    ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override {
        return m_file->preadv(iov, iovcnt, offset);
    }

    int fdatasync() override { return m_file->fdatasync(); }

    int fallocate(int mode, off_t offset, off_t len) override {
        return m_file->fallocate(mode, offset, len);
    }

    void set_auth_failed();
    int open_lower_layer(FileSystem::IFile *&file,
                         ImageConfigNS::LayerConfig &layer, int index);

    std::string m_exception;
    int m_status = 0; // 0: not started, 1: running, -1 exit

    size_t size;
    uint64_t num_lbas;
    uint32_t block_size;
    bool read_only = false;

private:
    struct GlobalFs &gfs;
    FileSystem::Prefetcher* m_prefetcher = nullptr;
    ImageConfigNS::GlobalConfig &gconf;
    ImageConfigNS::ImageConfig conf;
    std::list<BKDL::BkDownload *> dl_list;
    photon::join_handle *dl_thread_jh = nullptr;

    int init_image_file();
    void set_failed(std::string reason);
    LSMT::IFileRO *open_lowers(std::vector<ImageConfigNS::LayerConfig> &,
                               bool &);
    LSMT::IFileRW *open_upper(ImageConfigNS::UpperConfig &);
    FileSystem::IFile *__open_ro_file(const std::string &);
    FileSystem::IFile *__open_ro_remote(const std::string &dir,
                                        const std::string &, const uint64_t, int);
    void start_bk_dl_thread();
};
