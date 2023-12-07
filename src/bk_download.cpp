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
#include "bk_download.h"
#include <errno.h>
#include <list>
#include <set>
#include <string>
#include <thread>
#include <sys/file.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/alog-audit.h>
#include <photon/fs/localfs.h>
#include <photon/fs/throttled-file.h>
#include <photon/thread/thread.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <unistd.h>
#include "switch_file.h"
#include "image_file.h"
#include "tools/sha256file.h"

using namespace photon::fs;

static constexpr size_t ALIGNMENT = 4096;

namespace BKDL {

bool check_downloaded(const std::string &dir) {
    std::string fn = dir + "/" + COMMIT_FILE_NAME;
    auto lfs = photon::fs::new_localfs_adaptor();
    if (!lfs) {
        LOG_ERROR("new_localfs_adaptor() return NULL");
        return false;
    }
    DEFER({ delete lfs; });

    if (lfs->access(fn.c_str(), 0) == 0)
        return true;
    return false;
}

static std::set<std::string> lock_files;

void BkDownload::switch_to_local_file() {
    std::string path = dir + "/" + COMMIT_FILE_NAME;
    ((ISwitchFile *)sw_file)->set_switch_file(path.c_str());
    LOG_DEBUG("set switch done. (localpath: `)", path);
}

bool BkDownload::download_done() {
    auto lfs = new_localfs_adaptor();
    if (!lfs) {
        LOG_ERROR("new_localfs_adaptor() return NULL");
        return false;
    }
    DEFER({ delete lfs; });

    std::string old_name, new_name;
    old_name = dir + "/" + DOWNLOAD_TMP_NAME;
    new_name = dir + "/" + COMMIT_FILE_NAME;

    // verify sha256
    photon::semaphore done;
    std::string shares;
    std::thread sha256_thread([&]() {
        shares = sha256sum(old_name.c_str());
        done.signal(1);
    });
    sha256_thread.detach();
    // wait verify finish
    done.wait(1);

    if (shares != digest) {
        LOG_ERROR("verify checksum ` failed (expect: `, got: `)", old_name, digest, shares);
        force_download = true; // force redownload next time
        return false;
    }

    int ret = lfs->rename(old_name.c_str(), new_name.c_str());
    if (ret != 0) {
        LOG_ERRNO_RETURN(0, false, "rename(`,`) failed", old_name, new_name);
    }
    LOG_INFO("download verify done. rename(`,`) success", old_name, new_name);
    return true;
}

bool BkDownload::download() {
    if (check_downloaded(dir)) {
        switch_to_local_file();
        return true;
    }

    if (download_blob()) {
        if (!download_done())
            return false;
        switch_to_local_file();
        return true;
    }
    return false;
}

bool BkDownload::lock_file() {
    if (lock_files.find(dir) != lock_files.end()) {
        LOG_WARN("failed to lock download path:`", dir);
        return false;
    }
    lock_files.insert(dir);
    return true;
}

void BkDownload::unlock_file() {
    lock_files.erase(dir);
}

bool BkDownload::download_blob() {
    std::string dl_file_path = dir + "/" + DOWNLOAD_TMP_NAME;
    try_cnt--;
    IFile *src = src_file;
    if (limit_MB_ps > 0) {
        ThrottleLimits limits;
        limits.R.throughput = limit_MB_ps * 1024UL * 1024; // MB
        limits.R.block_size = 1024UL * 1024;
        limits.time_window = 1UL;
        src = new_throttled_file(src, limits);
    }
    DEFER({
        if (limit_MB_ps > 0)
            delete src;
    });

    auto dst = open_localfile_adaptor(dl_file_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (dst == nullptr) {
        LOG_ERRNO_RETURN(0, false, "failed to open dst file `", dl_file_path.c_str());
    }
    DEFER(delete dst;);
    dst->ftruncate(file_size);

    size_t bs = block_size;
    off_t offset = 0;
    void *buff = nullptr;
    // buffer allocate, with 4K alignment
    ::posix_memalign(&buff, ALIGNMENT, bs);
    if (buff == nullptr)
        LOG_ERRNO_RETURN(0, false, "failed to allocate buffer with ", VALUE(bs));
    DEFER(free(buff));

    LOG_INFO("download blob start. (`)", url);
    while (offset < (ssize_t)file_size) {
        if (running != 1) {
            LOG_INFO("image file exit when background downloading");
            return false;
        }
        if (!force_download) {
            // check aleady downloaded.
            auto hole_pos = dst->lseek(offset, SEEK_HOLE);
            if (hole_pos >= offset + (ssize_t)bs) {
                // alread downloaded
                offset += bs;
                continue;
            }
        }

        int retry = 2;
        auto count = bs;
        if (offset + count > file_size)
            count = file_size - offset;
    again_read:
        if (!(retry--))
            LOG_ERROR_RETURN(EIO, false, "failed to read at ", VALUE(offset), VALUE(count));
        ssize_t rlen;
        {
            SCOPE_AUDIT("bk_download", AU_FILEOP(url, offset, rlen));
            rlen = src->pread(buff, bs, offset);
        }
        if (rlen < 0) {
            LOG_WARN("failed to read at ", VALUE(offset), VALUE(count), VALUE(errno), " retry...");
            goto again_read;
        }
        retry = 2;
    again_write:
        if (!(retry--))
            LOG_ERROR_RETURN(EIO, false, "failed to write at ", VALUE(offset), VALUE(count));
        auto wlen = dst->pwrite(buff, count, offset);
        // but once write lenth larger than read length treats as OK
        if (wlen < rlen) {
            LOG_WARN("failed to write at ", VALUE(offset), VALUE(count), VALUE(errno), " retry...");
            goto again_write;
        }
        offset += count;
    }
    LOG_INFO("download blob done. (`)", dl_file_path);
    return true;
}

void bk_download_proc(std::list<BKDL::BkDownload *> &dl_list, uint64_t delay_sec, int &running) {
    LOG_INFO("BACKGROUND DOWNLOAD THREAD STARTED.");
    uint64_t time_st = photon::now;
    while (photon::now - time_st < delay_sec * 1000000) {
        photon::thread_usleep(200 * 1000);
        if (running != 1)
            break;
    }

    while (!dl_list.empty()) {
        if (running != 1) {
            LOG_WARN("image exited, background download exit...");
            break;
        }
        photon::thread_usleep(200 * 1000);

        BKDL::BkDownload *dl_item = dl_list.front();
        dl_list.pop_front();

        LOG_INFO("start downloading for dir `", dl_item->dir);

        if (!dl_item->lock_file()) {
            dl_list.push_back(dl_item);
            continue;
        }

        bool succ = dl_item->download();
        dl_item->unlock_file();

        if (running != 1) {
            LOG_WARN("image exited, background download exit...");
            delete dl_item;
            break;
        }

        if (!succ && dl_item->try_cnt > 0) {
            dl_list.push_back(dl_item);
            LOG_WARN("download failed, push back to download queue and retry `", dl_item->dir);
            continue;
        }
        LOG_DEBUG("finish downloading or no retry any more: `, retry_cnt: `", dl_item->dir,
                  dl_item->try_cnt);
        delete dl_item;
    }

    if (!dl_list.empty()) {
        LOG_INFO("DOWNLOAD THREAD EXITED in advance, delete dl_list.");
        while (!dl_list.empty()) {
            BKDL::BkDownload *dl_item = dl_list.front();
            dl_list.pop_front();
            delete dl_item;
        }
    }
    LOG_INFO("BACKGROUND DOWNLOAD THREAD EXIT.");
}

} // namespace BKDL
