
/*
 * sure_file.cpp
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

#include <limits.h>
#include <unistd.h>
#include "overlaybd/alog-stdstring.h"
#include "overlaybd/alog.h"
#include "overlaybd/fs/localfs.h"
#include "overlaybd/photon/thread.h"
#include "image_file.h"
#include "sure_file.h"

namespace FileSystem {
class SureFile : public ForwardFile_Ownership {
public:
    SureFile() = delete;
    SureFile(IFile *src_file, ImageFile *image_file, bool ownership)
        : ForwardFile_Ownership(src_file, ownership), m_ifile(image_file) {
    }

private:
    ImageFile *m_ifile = nullptr;

    void io_sleep(uint64_t &try_cnt) {
        if (try_cnt < 100)
            photon::thread_usleep(200); // 200us
        else
            photon::thread_usleep(2000); // 2ms

        if (try_cnt > 30000)
            photon::thread_sleep(1); // 1sec
        try_cnt++;
    }

    void io_hand() {
        while (m_ifile && m_ifile->m_status >= 0) {
            LOG_ERROR("write(...) incorrect, io hang here!");
            photon::thread_sleep(300);
        }
    }

public:
    virtual ssize_t write(const void *buf, size_t count) override {
        size_t done_cnt = 0;
        while (done_cnt < count) {
            ssize_t ret = m_file->write((char *)buf + done_cnt, count - done_cnt);
            if (ret > 0)
                done_cnt += ret;
            if (done_cnt == count)
                return count;
            if (done_cnt > count) {
                LOG_ERROR("write(...), done_cnt(`)>count(`), ret:`, errno:`, need io hang",
                          done_cnt, count, ret, errno);
                io_hand();
            }

            if (ret == -1 && errno == EINTR) {
                LOG_INFO("write(...), errno:EINTR, need continue try.");
                continue;
            } else {
                LOG_ERROR("write(...), done_cnt(`)>count(`), ret:`, errno:`, need io hang",
                          done_cnt, count, ret, errno);
                io_hand();
            }
        }
        return done_cnt;
    }

    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {
        uint64_t try_cnt = 0;
        size_t got_cnt = 0;
        auto time_st = photon::now;
        while (m_ifile && m_ifile->m_status >= 0 && photon::now - time_st < 61 * 1000 * 1000) {
            // exit on image in exit status, or timeout
            ssize_t ret = m_file->pread((char *)buf + got_cnt, count - got_cnt, offset + got_cnt);
            if (ret > 0)
                got_cnt += ret;
            if (got_cnt == count)
                return count;

            if ((ret < 0) && (!m_ifile->m_status < 1) && (errno == EPERM)) {
                // exit when booting. after boot, hang.
                m_ifile->set_auth_failed();
                LOG_ERROR_RETURN(0, -1, "authentication failed during image boot.");
            }

            if (got_cnt > count) {
                LOG_ERROR("pread(,`,`) return `. got_cnt:` > count:`, restart pread.", count,
                          offset, ret, got_cnt, count);
                got_cnt = 0;
            }

            io_sleep(try_cnt);

            if (try_cnt % 300 == 0) {
                LOG_ERROR("pread read partial data. count:`, offset:`, ret:`, got_cnt:`, errno:`",
                          count, offset, ret, got_cnt, errno);
            }
        }
        return -1;
    }
};
} // namespace FileSystem

FileSystem::IFile *new_sure_file(FileSystem::IFile *src_file, ImageFile *image_file,
                                 bool ownership) {
    if (!src_file) {
        LOG_ERROR("failed to new_sure_file(null)");
        return nullptr;
    }
    return new FileSystem::SureFile(src_file, image_file, ownership);
}

FileSystem::IFile *new_sure_file_by_path(const char *file_path, int open_flags,
                                         ImageFile *image_file, bool ownership) {
    auto file = FileSystem::open_localfile_adaptor(file_path, open_flags, 0644, 0);
    return new_sure_file(file, image_file, ownership);
}