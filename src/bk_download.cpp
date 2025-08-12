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
    auto tracer = overlaybd_otel::get_tracer("overlaybd");
    auto parent_span = opentelemetry::trace::Tracer::GetCurrentSpan();
    opentelemetry::trace::StartSpanOptions options;
    options.parent = parent_span->GetContext();
    auto scope = tracer->StartActiveSpan("overlaybd.download.verify_and_commit", options);
    auto span = opentelemetry::trace::Tracer::GetCurrentSpan();

    auto lfs = new_localfs_adaptor();
    if (!lfs) {
        span->SetAttribute("error", "failed_to_create_fs_adaptor");
        span->End();
        LOG_ERROR("new_localfs_adaptor() return NULL");
        return false;
    }
    DEFER({ delete lfs; });

    std::string old_name, new_name;
    old_name = dir + "/" + DOWNLOAD_TMP_NAME;
    new_name = dir + "/" + COMMIT_FILE_NAME;
    span->SetAttribute("temp_file", old_name);
    span->SetAttribute("commit_file", new_name);

    // verify sha256
    photon::semaphore done;
    std::string shares;
    opentelemetry::trace::StartSpanOptions verify_options;
    verify_options.parent = opentelemetry::trace::Tracer::GetCurrentSpan()->GetContext();
    auto verify_scope = tracer->StartActiveSpan("sha256_verification", verify_options);
    auto verify_span = opentelemetry::trace::Tracer::GetCurrentSpan();
    verify_span->SetAttribute("expected_digest", digest);

    std::thread sha256_thread([&]() {
        shares = sha256sum(old_name.c_str());
        done.signal(1);
    });
    sha256_thread.detach();
    // wait verify finish
    done.wait(1);

    verify_span->SetAttribute("actual_digest", shares);
    verify_span->SetAttribute("success", shares == digest);
    verify_span->End();

    if (shares != digest) {
        span->SetAttribute("error", "checksum_mismatch");
        span->SetAttribute("expected_digest", digest);
        span->SetAttribute("actual_digest", shares);
        span->End();
        LOG_ERROR("verify checksum ` failed (expect: `, got: `)", old_name, digest, shares);
        force_download = true; // force redownload next time
        return false;
    }

    opentelemetry::trace::StartSpanOptions rename_options;
    rename_options.parent = opentelemetry::trace::Tracer::GetCurrentSpan()->GetContext();
    auto rename_scope = tracer->StartActiveSpan("rename_to_commit", rename_options);
    auto rename_span = opentelemetry::trace::Tracer::GetCurrentSpan();
    int ret = lfs->rename(old_name.c_str(), new_name.c_str());
    rename_span->SetAttribute("success", ret == 0);

    if (ret != 0) {
        span->SetAttribute("error", "rename_failed");
        span->End();
        LOG_ERRNO_RETURN(0, false, "rename(`,`) failed", old_name, new_name);
    }

    span->SetAttribute("success", true);
    LOG_INFO("download verify done. rename(`,`) success", old_name, new_name);
    span->End();
    return true;
}

bool BkDownload::download() {
    auto tracer = overlaybd_otel::get_tracer("overlaybd");
    // Get current span context from parent if it exists
    auto parent_span = opentelemetry::trace::Tracer::GetCurrentSpan();
    opentelemetry::trace::StartSpanOptions options;
    options.parent = parent_span->GetContext();
    auto scope = tracer->StartActiveSpan("overlaybd.download.lifecycle", options);
    auto span = opentelemetry::trace::Tracer::GetCurrentSpan();

    span->SetAttribute("url", url);
    span->SetAttribute("dir", dir);
    span->SetAttribute("file_size", file_size);

    if (check_downloaded(dir)) {
        opentelemetry::trace::StartSpanOptions local_options;
        local_options.parent = opentelemetry::trace::Tracer::GetCurrentSpan()->GetContext();
        auto local_scope = tracer->StartActiveSpan("overlaybd.download.switch_to_local", local_options);
        auto local_span = opentelemetry::trace::Tracer::GetCurrentSpan();
        switch_to_local_file();
        span->SetAttribute("from_cache", true);
        span->End();
        return true;
    }

    span->SetAttribute("from_cache", false);
    bool success = false;
    if (download_blob()) {
        opentelemetry::trace::StartSpanOptions verify_options;
        verify_options.parent = opentelemetry::trace::Tracer::GetCurrentSpan()->GetContext();
        auto verify_scope = tracer->StartActiveSpan("overlaybd.download.verify", verify_options);
        auto verify_span = opentelemetry::trace::Tracer::GetCurrentSpan();
        if (!download_done()) {
            verify_span->SetAttribute("success", false);
            span->SetAttribute("success", false);
            span->End();
            return false;
        }
        verify_span->SetAttribute("success", true);
        verify_span->End();

        opentelemetry::trace::StartSpanOptions switch_options;
        switch_options.parent = opentelemetry::trace::Tracer::GetCurrentSpan()->GetContext();
        auto switch_scope = tracer->StartActiveSpan("overlaybd.download.switch_to_local", switch_options);
        auto switch_span = opentelemetry::trace::Tracer::GetCurrentSpan();
        switch_to_local_file();
        success = true;
    }

    span->SetAttribute("success", success);
    span->End();
    return success;
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
    auto tracer = overlaybd_otel::get_tracer("overlaybd");
    auto parent_span = opentelemetry::trace::Tracer::GetCurrentSpan();
    opentelemetry::trace::StartSpanOptions options;
    options.parent = parent_span->GetContext();
    auto scope = tracer->StartActiveSpan("overlaybd.download.blob", options);
    auto span = opentelemetry::trace::Tracer::GetCurrentSpan();

    std::string dl_file_path = dir + "/" + DOWNLOAD_TMP_NAME;
    span->SetAttribute("download_path", dl_file_path);
    span->SetAttribute("try_count", try_cnt);
    try_cnt--;

    IFile *src = src_file;
    if (limit_MB_ps > 0) {
        ThrottleLimits limits;
        limits.R.throughput = limit_MB_ps * 1024UL * 1024; // MB
        limits.R.block_size = 1024UL * 1024;
        limits.time_window = 1UL;
        src = new_throttled_file(src, limits);
        span->SetAttribute("throttle_limit_mbps", limit_MB_ps);
    }
    DEFER({
        if (limit_MB_ps > 0)
            delete src;
    });

    auto dst = open_localfile_adaptor(dl_file_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (dst == nullptr) {
        span->SetAttribute("error", "failed_to_open_dst");
        span->End();
        LOG_ERRNO_RETURN(0, false, "failed to open dst file `", dl_file_path.c_str());
    }
    DEFER(delete dst;);
    dst->ftruncate(file_size);

    size_t bs = block_size;
    off_t offset = 0;
    void *buff = nullptr;
    // buffer allocate, with 4K alignment
    ::posix_memalign(&buff, ALIGNMENT, bs);
    if (buff == nullptr) {
        span->SetAttribute("error", "failed_to_allocate_buffer");
        span->End();
        LOG_ERRNO_RETURN(0, false, "failed to allocate buffer with ", VALUE(bs));
    }
    DEFER(free(buff));

    LOG_INFO("download blob start. (`)", url);
    uint64_t total_bytes_read = 0;
    uint64_t total_bytes_written = 0;
    uint64_t retries = 0;

    while (offset < (ssize_t)file_size) {
        if (running != 1) {
            span->SetAttribute("error", "download_interrupted");
            span->End();
            LOG_INFO("image file exit when background downloading");
            return false;
        }
        if (!force_download) {
            // check aleady downloaded.
            auto hole_pos = dst->lseek(offset, SEEK_HOLE);
            if (hole_pos >= offset + (ssize_t)bs) {
                // alread downloaded
                offset += bs;
                total_bytes_written += bs;
                continue;
            }
        }

        int retry = 2;
        auto count = bs;
        if (offset + count > file_size)
            count = file_size - offset;
    again_read:
        if (!(retry--)) {
            span->SetAttribute("error", "max_read_retries_exceeded");
            span->SetAttribute("failed_offset", offset);
            span->End();
            LOG_ERROR_RETURN(EIO, false, "failed to read at ", VALUE(offset), VALUE(count));
        }
        ssize_t rlen;
        {
            auto read_scope = tracer->StartActiveSpan("overlaybd.download.read_block");
            auto read_span = opentelemetry::trace::Tracer::GetCurrentSpan();
            read_span->SetAttribute("offset", offset);
            read_span->SetAttribute("size", count);
            SCOPE_AUDIT("bk_download", AU_FILEOP(url, offset, rlen));
            rlen = src->pread(buff, bs, offset);
            if (rlen >= 0) {
                read_span->SetAttribute("bytes_read", rlen);
                total_bytes_read += rlen;
            }
            read_span->End();
        }
        if (rlen < 0) {
            retries++;
            LOG_WARN("failed to read at ", VALUE(offset), VALUE(count), VALUE(errno), " retry...");
            goto again_read;
        }
        retry = 2;
    again_write:
        if (!(retry--)) {
            span->SetAttribute("error", "max_write_retries_exceeded");
            span->SetAttribute("failed_offset", offset);
            span->End();
            LOG_ERROR_RETURN(EIO, false, "failed to write at ", VALUE(offset), VALUE(count));
        }
        auto write_scope = tracer->StartActiveSpan("overlaybd.download.write_block");
        auto write_span = opentelemetry::trace::Tracer::GetCurrentSpan();
        write_span->SetAttribute("offset", offset);
        write_span->SetAttribute("size", count);
        auto wlen = dst->pwrite(buff, count, offset);
        if (wlen >= 0) {
            write_span->SetAttribute("bytes_written", wlen);
            total_bytes_written += wlen;
        }
        write_span->End();
        // but once write lenth larger than read length treats as OK
        if (wlen < rlen) {
            retries++;
            LOG_WARN("failed to write at ", VALUE(offset), VALUE(count), VALUE(errno), " retry...");
            goto again_write;
        }
        offset += count;
    }

    span->SetAttribute("total_bytes_read", total_bytes_read);
    span->SetAttribute("total_bytes_written", total_bytes_written);
    span->SetAttribute("total_retries", retries);
    span->SetAttribute("success", true);
    LOG_INFO("download blob done. (`)", dl_file_path);
    span->End();
    return true;
}

void bk_download_proc(std::list<BKDL::BkDownload *> &dl_list, uint64_t delay_sec, int &running) {
    auto tracer = overlaybd_otel::get_tracer("overlaybd");
    auto scope = tracer->StartActiveSpan("background_download_process");
    auto span = opentelemetry::trace::Tracer::GetCurrentSpan();
    
    span->SetAttribute("delay_seconds", delay_sec);
    span->SetAttribute("initial_queue_size", dl_list.size());
    
    LOG_INFO("BACKGROUND DOWNLOAD THREAD STARTED.");
    uint64_t time_st = photon::now;
    while (photon::now - time_st < delay_sec * 1000000) {
        photon::thread_usleep(200 * 1000);
        if (running != 1) {
            span->SetAttribute("early_exit", "delay_interrupted");
            break;
        }
    }

    while (!dl_list.empty()) {
        if (running != 1) {
            span->SetAttribute("early_exit", "image_exited");
            LOG_WARN("image exited, background download exit...");
            break;
        }
        photon::thread_usleep(200 * 1000);

        BKDL::BkDownload *dl_item = dl_list.front();
        dl_list.pop_front();

        auto dl_scope = tracer->StartActiveSpan("download_item");
        auto dl_span = opentelemetry::trace::Tracer::GetCurrentSpan();
        dl_span->SetAttribute("directory", dl_item->dir);
        dl_span->SetAttribute("retry_count", dl_item->try_cnt);

        LOG_INFO("start downloading for dir `", dl_item->dir);

        if (!dl_item->lock_file()) {
            dl_span->SetAttribute("status", "lock_failed");
            dl_span->End();
            dl_list.push_back(dl_item);
            continue;
        }

        bool succ = dl_item->download();
        dl_item->unlock_file();

        if (running != 1) {
            dl_span->SetAttribute("status", "interrupted");
            dl_span->End();
            LOG_WARN("image exited, background download exit...");
            delete dl_item;
            break;
        }

        if (!succ && dl_item->try_cnt > 0) {
            dl_span->SetAttribute("status", "retry");
            dl_span->End();
            dl_list.push_back(dl_item);
            LOG_WARN("download failed, push back to download queue and retry `", dl_item->dir);
            continue;
        }
        
        dl_span->SetAttribute("status", succ ? "success" : "failed");
        dl_span->End();
        
        LOG_DEBUG("finish downloading or no retry any more: `, retry_cnt: `", dl_item->dir,
                  dl_item->try_cnt);
        delete dl_item;
    }

    if (!dl_list.empty()) {
        span->SetAttribute("unfinished_downloads", dl_list.size());
        LOG_INFO("DOWNLOAD THREAD EXITED in advance, delete dl_list.");
        while (!dl_list.empty()) {
            BKDL::BkDownload *dl_item = dl_list.front();
            dl_list.pop_front();
            delete dl_item;
        }
    }
    LOG_INFO("BACKGROUND DOWNLOAD THREAD EXIT.");
    span->End();
}

} // namespace BKDL
