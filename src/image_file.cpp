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
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/aligned-file.h>
#include <photon/fs/localfs.h>
#include "overlaybd/lsmt/file.h"
#include "overlaybd/zfile/zfile.h"
#include "config.h"
#include "image_file.h"
#include "switch_file.h"
#include "overlaybd/gzip/gz.h"
#include "overlaybd/gzindex/gzfile.h"
#include "overlaybd/tar/tar_file.h"

#define PARALLEL_LOAD_INDEX 32
using namespace photon::fs;

#define SET_LOCAL_DIR 118
#define SET_SIZE 119

IFile *ImageFile::__open_ro_file(const std::string &path) {
    int flags = O_RDONLY;

    LOG_INFO("open ro file: `", path);
    int ioengine = image_service.global_conf.ioEngine();
    if (ioengine > 2) {
        LOG_WARN("invalid ioengine: `, set to psync", ioengine);
        ioengine = 0;
    }
    if (ioengine == ioengine_libaio) {
        flags |= O_DIRECT;
        LOG_DEBUG("`: flag add O_DIRECT", path);
    }

    auto file = open_localfile_adaptor(path.c_str(), flags, 0644, ioengine);
    if (!file) {
        set_failed("failed to open local file " + path);
        LOG_ERRNO_RETURN(0, nullptr, "open(`) failed", path);
    }

    if (flags & O_DIRECT) {
        LOG_DEBUG("create aligned file. IO_FLAGS: `", flags);
        auto aligned_file = new_aligned_file_adaptor(file, ALIGNMENT_4K, true, true);
        if (!aligned_file) {
            set_failed("failed to open aligned_file_adaptor " + path);
            delete file;
            LOG_ERRNO_RETURN(0, nullptr, "new_aligned_file_adaptor(`) failed", path);
        }
        file = aligned_file;
    }

    auto tar_file = new_tar_file_adaptor(file);
    if (!tar_file) {
        set_failed("failed to open file as tar file " + path);
        delete file;
        LOG_ERROR_RETURN(0, nullptr, "new_tar_file_adaptor(`) failed", path);
    }
    file = tar_file;
    // set to local, no need to switch, for zfile and audit
    ISwitchFile *switch_file = new_switch_file(file, true, path.c_str());
    if (!switch_file) {
        set_failed("failed to open switch file " + path);
        delete file;
        LOG_ERRNO_RETURN(0, nullptr, "new_switch_file(`) failed", path);
    }
    file = switch_file;

    return file;
}

IFile *ImageFile::__open_ro_target_file(const std::string &path) {
    auto file = open_localfile_adaptor(path.c_str(), O_RDONLY, 0644, 0);
    if (!file) {
        set_failed("failed to open local data file " + path);
        LOG_ERRNO_RETURN(0, nullptr, "open(`) failed", path);
    }
    return file;
}

IFile *ImageFile::__open_ro_target_remote(const std::string &dir, const std::string &data_digest,
                                               const uint64_t size, int layer_index) {
    std::string url;

    if (conf.repoBlobUrl() == "") {
        set_failed("empty repoBlobUrl");
        LOG_ERROR_RETURN(0, nullptr, "empty repoBlobUrl for remote layer");
    }
    url = conf.repoBlobUrl();

    if (url[url.length() - 1] != '/')
        url += "/";
    url += data_digest;

    LOG_INFO("open file from remotefs: `", url);
    IFile *remote_file = image_service.global_fs.remote_fs->open(url.c_str(), O_RDONLY);
    if (!remote_file) {
        if (errno == EPERM)
            set_auth_failed();
        else
            set_failed("failed to open remote file " + url);
        LOG_ERROR_RETURN(0, nullptr, "failed to open remote file `", url);
    }

    return remote_file;
}

IFile *ImageFile::__open_ro_remote(const std::string &dir, const std::string &digest,
                                   const uint64_t size, int layer_index) {
    std::string url;

    if (conf.repoBlobUrl() == "") {
        set_failed("empty repoBlobUrl");
        LOG_ERROR_RETURN(0, nullptr, "empty repoBlobUrl for remote layer");
    }
    url = conf.repoBlobUrl();

    if (url[url.length() - 1] != '/')
        url += "/";
    url += digest;

    LOG_INFO("open file from remotefs: `, size: `", url, size);
    IFile *remote_file = image_service.global_fs.remote_fs->open(url.c_str(), O_RDONLY);
    if (!remote_file) {
        std::string err_msg = "failed to open remote file " + url + ": ";
        if (errno == EPERM || errno == EACCES) {
            err_msg += "Authentication failed";
        } else if (errno == ENOTCONN) {
            err_msg += "Connection failed";
        } else if (errno == ETIMEDOUT) {
            err_msg += "Get meta timedout";
        } else if (errno == ENOENT) {
            err_msg += "No such file or directory";
        } else if (errno == EBUSY) {
            err_msg += "Too many requests";
        } else if (errno == EIO) {
            err_msg += "Unexpected response";
        } else {
            err_msg += std::string(strerror(errno));
        }
        set_failed(err_msg);
        LOG_ERRNO_RETURN(0, nullptr, err_msg);
    }
    remote_file->ioctl(SET_SIZE, size);
    remote_file->ioctl(SET_LOCAL_DIR, dir);

    IFile *tar_file = new_tar_file_adaptor(remote_file);
    if (!tar_file) {
        set_failed("failed to open remote file as tar file " + url);
        delete remote_file;
        LOG_ERROR_RETURN(0, nullptr, "failed to open remote file as tar file `", url);
    }

    ISwitchFile *switch_file = new_switch_file(tar_file, false, url.c_str());
    if (!switch_file) {
        set_failed("failed to open switch file " + url);
        delete tar_file;
        LOG_ERROR_RETURN(0, nullptr, "failed to open switch file `", url);
    }

    if (conf.HasMember("download") && conf.download().enable() == 1) {
        // download from registry, verify sha256 after downloaded.
        IFile *srcfile = image_service.global_fs.srcfs->open(url.c_str(), O_RDONLY);
        if (srcfile == nullptr) {
            LOG_WARN("failed to open source file, ignore download");
        } else {
            BKDL::BkDownload *obj =
                new BKDL::BkDownload(switch_file, srcfile, size, dir, digest, url, m_status,
                    conf.download().maxMBps(), conf.download().tryCnt(), conf.download().blockSize());
            LOG_DEBUG("add to download list for `", dir);
            dl_list.push_back(obj);
        }
    }

    return switch_file;
}

void ImageFile::start_bk_dl_thread() {
    if (dl_list.empty()) {
        LOG_INFO("no need to download");
        return;
    }

    uint64_t extra_range = conf.download().delayExtra();
    extra_range = (extra_range <= 0) ? 30 : extra_range;
    uint64_t delay_sec = (rand() % extra_range) + conf.download().delay();
    LOG_INFO("background download is enabled, delay `, maxMBps `, tryCnt `, blockSize `",
             delay_sec, conf.download().maxMBps(), conf.download().tryCnt(), conf.download().blockSize());
    dl_thread_jh = photon::thread_enable_join(
        photon::thread_create11(&BKDL::bk_download_proc, dl_list, delay_sec, m_status));
}

struct ParallelOpenTask {
    std::vector<IFile *> &files;
    int eno = 0;
    int i = 0, nlayers;
    std::vector<ImageConfigNS::LayerConfig> &layers;

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

    ParallelOpenTask(std::vector<IFile *> &files, size_t nlayers,
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

int ImageFile::open_lower_layer(IFile *&file, ImageConfigNS::LayerConfig &layer,
                                int index) {
    std::string opened;
    if (layer.file() != "") {
        opened = layer.file();
        file = __open_ro_file(opened);
    } else {
        // open downloaded blob or remote blob
        if (BKDL::check_downloaded(layer.dir())) {
            opened = layer.dir() + "/" + COMMIT_FILE_NAME;
            file = __open_ro_file(opened);
        } else {
            auto sealed = layer.dir() + "/" + SEALED_FILE_NAME;
            if (::access(sealed.c_str(), 0) == 0) {
                // open sealed blob
                opened = sealed;
                file = __open_ro_file(opened);
            } else {
                opened = layer.digest();
                file = __open_ro_remote(layer.dir(), layer.digest(), layer.size(), index);
            }
        }
    }

    if (file == nullptr) {
        return -1;
    }

    if (m_prefetcher != nullptr) {
        file = m_prefetcher->new_prefetch_file(file, index);
    }

    IFile *target_file = nullptr;

    if (layer.targetFile() != "") {
        LOG_INFO("open local data file `", layer.targetFile());
        target_file = __open_ro_target_file(layer.targetFile());
    } else if (layer.targetDigest() != "") {
        LOG_INFO("open remote data file `", layer.targetDigest());
        target_file = __open_ro_target_remote(layer.dir(), layer.targetDigest(), 0, index);
    }
    if (layer.gzipIndex() != "") {
        auto gz_index = open_localfile_adaptor(layer.gzipIndex().c_str(), O_RDONLY, 0644, 0);
        if (!gz_index) {
            set_failed("failed to open gzip index " + layer.gzipIndex());
            LOG_ERRNO_RETURN(0, -1, "open(`) failed", layer.gzipIndex());
        }
        target_file = new_gzfile(target_file, gz_index, true);
        if (image_service.global_conf.gzipCacheConfig().enable() && layer.targetDigest() != "") {
            target_file = image_service.global_fs.gzcache_fs->
                          open_cached_gzip_file(target_file, layer.targetDigest().c_str());
        }
    }
    if (target_file != nullptr) {
        file = LSMT::open_warpfile_ro(file, target_file, true);
    }
    if (file != nullptr) {
        LOG_DEBUG("layer index: `, open(`) success", index, opened);
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
    std::vector<IFile *> files;
    files.resize(lowers.size(), nullptr);
    auto n = std::min(PARALLEL_LOAD_INDEX, (int)lowers.size());
    LOG_DEBUG("create ` photon threads to open lowers", n);

    ParallelOpenTask tm(files, lowers.size(), lowers);
    for (auto i = 0; i < n; ++i) {
        ths[i] =
            photon::thread_enable_join(photon::thread_create11(&do_parallel_open_files, this, tm));
    }

    for (int i = 0; i < n; i++) {
        photon::thread_join(ths[i]);
    }

    for (size_t i = 0; i < files.size(); i++) {
        if (files[i] == NULL) {
            LOG_ERROR("layer index ` open failed, exit.", i);
            if (m_exception == "")
                m_exception = "failed to open layer " + std::to_string(i);

            goto ERROR_EXIT;
        }
    }
    ret = LSMT::open_files_ro((IFile **)&(files[0]), lowers.size(), true);
    if (!ret) {
        LOG_ERROR("LSMT::open_files_ro(files, `, `) return NULL", lowers.size(), true);
        goto ERROR_EXIT;
    }
    LOG_INFO("LSMT::open_files_ro(files, `) success", lowers.size());

    if (m_prefetcher != nullptr) {
        m_prefetcher->replay();
    }

    return ret;

ERROR_EXIT:
    if (m_exception == "") {
        m_exception = "failed to create overlaybd device";
    }
    for (size_t i = 0; i < lowers.size(); i++) {
        if (files[i] != NULL)
            delete files[i];
    }
    has_error = true;
    return NULL;
}

LSMT::IFileRW *ImageFile::open_upper(ImageConfigNS::UpperConfig &upper) {
    IFile *data_file = NULL;
    IFile *idx_file = NULL;
    IFile *target_file = NULL;
    LSMT::IFileRW *ret = NULL;
    data_file = open_localfile_adaptor(upper.data().c_str(), O_RDWR, 0644);
    if (!data_file) {
        LOG_ERROR("open(`,flags), `:`", upper.data(), errno, strerror(errno));
        goto ERROR_EXIT;
    }

    idx_file = open_localfile_adaptor(upper.index().c_str(), O_RDWR, 0644);
    if (!idx_file) {
        LOG_ERROR("open(`,flags), `:`", upper.index(), errno, strerror(errno));
        goto ERROR_EXIT;
    }

    if (upper.target() != "") {
        LOG_INFO("turboOCIv1 upper layer : `, `, `, `", upper.index(), upper.data(), upper.target());
        target_file = open_localfile_adaptor(upper.target().c_str(), O_RDWR, 0644);
        if (!target_file) {
            LOG_ERROR("open(`,flags), `:`", upper.target(), errno, strerror(errno));
            goto ERROR_EXIT;
        }
        if (upper.gzipIndex() != "") {
            auto gzip_index = open_localfile_adaptor(upper.gzipIndex().c_str(), O_RDWR, 0644);
            if (!gzip_index) {
                LOG_ERROR("open(`,flags), `:`", upper.gzipIndex(), errno, strerror(errno));
                goto ERROR_EXIT;
            }
            target_file = new_gzfile(target_file, gzip_index);
        }
        ret = LSMT::open_warpfile_rw(idx_file, data_file, target_file, true);
        if (!ret) {
            LOG_ERROR("LSMT::open_warpfile_rw(`,`,`,`) return NULL",
                    (uint64_t)data_file, (uint64_t)idx_file, (uint64_t)target_file, true);
            goto ERROR_EXIT;
        }
    } else {
        LOG_INFO("overlaybd upper layer : ` , `", upper.index(), upper.data());
        ret = LSMT::open_file_rw(data_file, idx_file, true);
        if (!ret) {
            LOG_ERROR("LSMT::open_file_rw(`,`,`) return NULL",
                    (uint64_t)data_file, (uint64_t)idx_file, true);
            goto ERROR_EXIT;
        }
    }
    return ret;

ERROR_EXIT:
    delete data_file;
    delete idx_file;
    delete ret;
    return NULL;
}

int ImageFile::init_image_file() {
    LSMT::IFileRO *lower_file = nullptr;
    LSMT::IFileRW *upper_file = nullptr;
    LSMT::IFileRW *stack_ret = nullptr;
    ImageConfigNS::UpperConfig upper;
    bool record_no_download = false;
    bool has_error = false;
    auto lowers = conf.lowers();
    auto concurrency = image_service.global_conf.prefetchConfig().concurrency();

    if (conf.accelerationLayer() && !conf.recordTracePath().empty()) {
        LOG_ERROR("Cannot record trace while acceleration layer exists");
        goto ERROR_EXIT;

    } else if (conf.accelerationLayer() && !lowers.empty()) {
        std::string accel_layer = lowers.back().dir();
        lowers.pop_back();
        LOG_INFO("Acceleration layer found at `, ignore the last lower", accel_layer);

        std::string trace_file = accel_layer + "/trace";
        if (Prefetcher::detect_mode(trace_file) ==
            Prefetcher::Mode::Replay) {
            m_prefetcher = new_prefetcher(trace_file, concurrency);
        }

    } else if (!conf.recordTracePath().empty()) {
        auto mode = Prefetcher::detect_mode(conf.recordTracePath());
        if (mode != Prefetcher::Mode::Record && mode != Prefetcher::Mode::Replay) {
            LOG_ERROR("Prefetch: incorrect mode ` for prefetching", mode);
            goto ERROR_EXIT;
        }
        m_prefetcher = new_prefetcher(conf.recordTracePath(), concurrency);
        if (mode == Prefetcher::Mode::Record) {
            record_no_download = true;
        }
    }

    upper.CopyFrom(conf.upper(), upper.GetAllocator());
    lower_file = open_lowers(lowers, has_error);

    if (has_error) {
        // NOTE: lower_file is allowed to be NULL. In this case, there is only one layer.
        LOG_ERROR("open lower layer failed.");
        goto ERROR_EXIT;
    }

    if (upper.index() == "" || upper.data() == "") {
        LOG_INFO("RW layer path not set. return RO layers.");
        m_file = lower_file;
        read_only = true;
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
    read_only = false;

SUCCESS_EXIT:
    if (conf.download().enable() && !record_no_download) {
        start_bk_dl_thread();
    }
    return 1;

ERROR_EXIT:
    delete lower_file;
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
