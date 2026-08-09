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
#include "overlaybd/lsmt/index.h"
#include "overlaybd/zfile/zfile.h"
#include "config.h"
#include "image_file.h"
#include "switch_file.h"
#include "overlaybd/gzip/gz.h"
#include "overlaybd/gzindex/gzfile.h"
#include "overlaybd/tar/tar_file.h"
#ifdef PHOTON_ENABLE_RESIZE
#include <photon/fs/extfs/extfs.h>
#endif

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

void get_error_msg(int eno, std::string &err_msg) {
    if (eno == EPERM || eno == EACCES) {
        err_msg = "Authentication failed";
    } else if (eno == ENOTCONN) {
        err_msg = "Connection failed";
    } else if (eno == ETIMEDOUT) {
        err_msg = "Get meta timedout";
    } else if (eno == ENOENT) {
        err_msg = "No such file or directory";
    } else if (eno == EBUSY) {
        err_msg = "Too many requests";
    } else if (eno == EIO) {
        err_msg = "Unexpected response";
    } else {
        err_msg = std::string(strerror(eno));
    }
}

IFile *ImageFile::__open_ro_remote(const std::string &dir, const std::string &digest,
                                   const uint64_t size, int layer_index) {
    if (conf.repoBlobUrl() == "") {
        set_failed("empty repoBlobUrl");
        LOG_ERROR_RETURN(0, nullptr, "empty repoBlobUrl for remote layer");
    }
    estring url = estring().appends("/", conf.repoBlobUrl(),
                                    (conf.repoBlobUrl().back() != '/') ? "/" : "", digest);

    LOG_INFO("open file from remotefs: `, size: `", url, size);
    IFile *remote_file = image_service.global_fs.remote_fs->open(url.c_str(), O_RDONLY);
    if (!remote_file) {
        std::string err_msg;
        get_error_msg(errno, err_msg);
        set_failed("failed to open remote file ", url, ": ", err_msg);
        LOG_ERRNO_RETURN(0, nullptr, "failed to open remote file `: `", url, err_msg);
    }
    if (!dir.empty()) {
        remote_file->ioctl(SET_SIZE, size);
        remote_file->ioctl(SET_LOCAL_DIR, dir);
    } else {
        LOG_WARN(
            "local dir of layer ` (`) didn't set, skip background anyway",
            layer_index, digest.c_str());
    }

    IFile *tar_file = new_tar_file_adaptor(remote_file);
    if (!tar_file) {
        std::string err_msg;
        get_error_msg(errno, err_msg);
        set_failed("failed to open remote file as tar file ", url, ": ", err_msg);
        delete remote_file;
        LOG_ERRNO_RETURN(0, nullptr, "failed to open remote file as tar file `: `", url, err_msg);
    }

    ISwitchFile *switch_file = new_switch_file(tar_file, false, url.c_str());
    if (!switch_file) {
        set_failed("failed to open switch file ", url);
        delete tar_file;
        LOG_ERRNO_RETURN(0, nullptr, "failed to open switch file `", url);
    }

    if (conf.HasMember("download") && conf.download().enable() == 1 && !dir.empty()) {
        // download from registry, verify sha256 after downloaded.
        IFile *srcfile = image_service.global_fs.srcfs->open(url.c_str(), O_RDONLY);
        if (srcfile == nullptr) {
            LOG_WARN("failed to open source file, ignore download");
        } else {
            BKDL::BkDownload *obj = new BKDL::BkDownload(
                switch_file, srcfile, size, dir, digest, url, m_status, conf.download().maxMBps(),
                conf.download().tryCnt(), conf.download().blockSize());
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
    LOG_INFO("background download is enabled, delay `, maxMBps `, tryCnt `, blockSize `", delay_sec,
             conf.download().maxMBps(), conf.download().tryCnt(), conf.download().blockSize());
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

IFile *ImageFile::open_localfile(ImageConfigNS::LayerConfig &layer, std::string &opened) {
    if (layer.file() != "") {
        opened = layer.file();
        return __open_ro_file(opened);
    }
    if (BKDL::check_downloaded(layer.dir())) {
        opened = layer.dir() + "/" + COMMIT_FILE_NAME;
        return __open_ro_file(opened);
    }
    auto sealed = layer.dir() + "/" + SEALED_FILE_NAME;
    if (::access(sealed.c_str(), 0) == 0) {
        // open sealed blob
        opened = sealed;
        return __open_ro_file(opened);
    }
    return nullptr;
}
int ImageFile::open_lower_layer(IFile *&file, ImageConfigNS::LayerConfig &layer, int index) {
    std::string opened;
    file = open_localfile(layer, opened); // try to open localfile if downloaded
    if (file == nullptr) {
        opened = layer.digest();
        file = __open_ro_remote(layer.dir(), layer.digest(), layer.size(), index);
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
            target_file = image_service.global_fs.gzcache_fs->open_cached_gzip_file(
                target_file, layer.targetDigest().c_str());
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
    std::vector<IFile *> files; // layer0 ... layerN-1
    files.resize(lowers.size(), nullptr);
    auto n = std::min(PARALLEL_LOAD_INDEX, (int)lowers.size());
    LOG_DEBUG("create ` photon threads to open lowers", n);

    ParallelOpenTask tm(files, lowers.size(), lowers); // layer0 ... layerN-1
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
        LOG_INFO("turboOCIv1 upper layer : `, `, `, `", upper.index(), upper.data(),
                 upper.target());
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
            LOG_ERROR("LSMT::open_warpfile_rw(`,`,`,`) return NULL", (uint64_t)data_file,
                      (uint64_t)idx_file, (uint64_t)target_file, true);
            goto ERROR_EXIT;
        }
    } else {
        LOG_INFO("overlaybd upper layer : ` , `", upper.index(), upper.data());
        ret = LSMT::open_file_rw(data_file, idx_file, true);
        if (!ret) {
            LOG_ERROR("LSMT::open_file_rw(`,`,`) return NULL", (uint64_t)data_file,
                      (uint64_t)idx_file, true);
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
    auto lowers = conf.lowers(); // layer0 ... layerN-1
    auto concurrency = image_service.global_conf.prefetchConfig().concurrency();

    if (conf.accelerationLayer() && !conf.recordTracePath().empty()) {
        LOG_ERROR("Cannot record trace while acceleration layer exists");
        goto ERROR_EXIT;

    } else if (conf.accelerationLayer() && !lowers.empty()) {
        std::string accel_layer = lowers.back().dir();
        lowers.pop_back();
        LOG_INFO("Acceleration layer found at `, ignore the last lower", accel_layer);

        std::string trace_file = accel_layer + "/trace";
        if (Prefetcher::detect_mode(trace_file) == Prefetcher::Mode::Replay) {
            m_prefetcher = new_prefetcher(trace_file, concurrency);
        }

    } else if (!conf.recordTracePath().empty() && (::access(conf.recordTracePath().c_str(), F_OK) == 0)) {
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

    // have only RO layers
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

    // have only one RW layer
    if (!lower_file) {
        LOG_INFO("RO layers path not set. return RW layer.");
        m_file = upper_file;
        read_only = false;
        goto SUCCESS_EXIT;
    }

    // stack_files(..., ownership=true) will destruct lower_file and upper_file
    // immediately, but the read-only lower_file is needed by prefetcher, so we
    // have to use stack_files(..., ownership=false) instead.
    //
    // For this reason, lower_file and upper_file must be maintained until m_file
    // is deconstructed.
    stack_ret = LSMT::stack_files(upper_file, lower_file, false, false);
    if (!stack_ret) {
        LOG_ERROR("LSMT::stack_files(`, `)", (uint64_t)upper_file, true);
        goto ERROR_EXIT;
    }
    m_file = stack_ret;
    read_only = false;
    m_lower_file = lower_file;
    m_upper_file = upper_file;

SUCCESS_EXIT:
    if (conf.download().enable() && !record_no_download) {
        start_bk_dl_thread();
    }
    if (m_prefetcher && lower_file) {
        m_prefetcher->replay(lower_file);
    }
    return 1;

ERROR_EXIT:
    delete lower_file;
    delete upper_file;
    return -1;
}

int ImageFile::compact(IFile *as) {
    return ((LSMT::IFileRO*)m_file)->flatten(as);
}

void ImageFile::set_auth_failed() {
    if (m_status == 0) // only set exit in image boot phase
    {
        m_status = -1;
        m_exception = "Authentication failed";
    }
}

bool ImageFile::layer_config_match(ImageConfigNS::LayerConfig &a,
                                   ImageConfigNS::LayerConfig &b) {
    return a.gzipIndex() == b.gzipIndex() && a.file() == b.file() &&
           a.targetFile() == b.targetFile() && a.dir() == b.dir() &&
           a.digest() == b.digest() && a.targetDigest() == b.targetDigest() &&
           a.size() == b.size();
}

bool ImageFile::configs_match(ImageConfigNS::ImageConfig &a,
                              ImageConfigNS::ImageConfig &b) {
    auto a_lowers = a.lowers();
    auto b_lowers = b.lowers();
    if (a_lowers.size() != b_lowers.size())
        return false;
    for (size_t i = 0; i < a_lowers.size(); ++i) {
        if (!layer_config_match(a_lowers[i], b_lowers[i]))
            return false;
    }
    return a.upper().index() == b.upper().index() && a.upper().data() == b.upper().data();
}

int ImageFile::read_text_file(const std::string &path, std::string &out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return -1;
    DEFER(::close(fd));
    out.clear();
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        out.append(buf, n);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return 0;
}

int ImageFile::write_text_file_fsync(const std::string &path, const std::string &data) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            ::close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        return -1;
    }
    ::close(fd);

    // Fsync the parent directory so the create/replace is durable.
    auto slash = path.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return 0;
}

int ImageFile::install_canonical_config(const std::string &src_path) {
    auto lfs = photon::fs::new_localfs_adaptor();
    if (lfs == nullptr)
        return -1;
    DEFER(delete lfs);
    std::string tmp = config_path + ".restack.tmp";
    // Copy intent bytes to a same-dir temp then rename over canonical config.
    std::string bytes;
    if (read_text_file(src_path, bytes) != 0)
        return -1;
    if (write_text_file_fsync(tmp, bytes) != 0) {
        unlink(tmp.c_str());
        return -1;
    }
    if (lfs->rename(tmp.c_str(), config_path.c_str()) != 0) {
        unlink(tmp.c_str());
        return -1;
    }
    return 0;
}

int ImageFile::create_snapshot(const char *new_config_path) {
    // load new config file to get the snapshot layer path
    // open new upper layer
    // restack() current RW layer as snapshot layer
    photon::scoped_rwlock lock(m_io_lock, photon::WLOCK);

    if(!m_lower_file || !m_upper_file)
        LOG_ERROR_RETURN(0, -1, "Lower or upper layer is NULL.");

    const std::string intent_path = config_path + ".restack.intent";
    const std::string phase_path = config_path + ".restack.phase";

    // Finish a previously mutated restack whose canonical config publish may
    // have been interrupted (lost response / crash after restack).
    {
        std::string phase;
        if (read_text_file(phase_path, phase) == 0 && phase == "mutated") {
            ImageConfigNS::ImageConfig intent_cfg;
            if (!intent_cfg.ParseJSON(intent_path.c_str())) {
                LOG_ERROR_RETURN(0, -1,
                                 "Restack journal is mutated but intent config is unreadable; "
                                 "refusing further restacks until repaired.");
            }
            if (configs_match(conf, intent_cfg)) {
                if (install_canonical_config(intent_path) != 0) {
                    LOG_ERROR_RETURN(0, -1, "Failed to finish restack journal publish.");
                }
                unlink(phase_path.c_str());
                unlink(intent_path.c_str());
                return 0;
            }
            LOG_ERROR_RETURN(0, -1,
                             "Restack journal is mutated but live config does not match intent; "
                             "refusing a second restack.");
        }
        // Pre-mutation intent is safe to discard before starting a new attempt.
        if (phase == "prepared") {
            unlink(phase_path.c_str());
            unlink(intent_path.c_str());
        }
    }

    ImageConfigNS::ImageConfig new_cfg;
    LSMT::IFileRW *upper_file = nullptr;

    LOG_INFO("Load new config for restack.");
    if (!new_cfg.ParseJSON(new_config_path)) {
        LOG_ERROR_RETURN(0, -1, "Error parse new config json.");
    }

    // Idempotent retry: canonical stack already matches the requested next config.
    if (configs_match(conf, new_cfg)) {
        LOG_INFO("Restack request already applied; returning success.");
        unlink(phase_path.c_str());
        unlink(intent_path.c_str());
        return 0;
    }

    auto current_lowers = conf.lowers();
    auto new_lowers = new_cfg.lowers();
    if (new_lowers.size() != current_lowers.size() + 1) {
        LOG_ERROR_RETURN(0, -1,
                         "The new config must preserve all current lowers and append the current upper (current: `, new: `).",
                         current_lowers.size(), new_lowers.size());
    }
    for (size_t i = 0; i < current_lowers.size(); ++i) {
        auto &current = current_lowers[i];
        auto &next = new_lowers[i];
        if (next.gzipIndex() != current.gzipIndex() || next.file() != current.file() ||
            next.targetFile() != current.targetFile() || next.dir() != current.dir() ||
            next.digest() != current.digest() || next.targetDigest() != current.targetDigest() ||
            next.size() != current.size()) {
            LOG_ERROR_RETURN(0, -1, "The new config changes existing lower layer `.", i);
        }
    }

    auto current_upper = conf.upper();
    auto &snapshot_lower = new_lowers.back();
    if (snapshot_lower.file() != current_upper.data() || !snapshot_lower.gzipIndex().empty() ||
        !snapshot_lower.targetFile().empty() || !snapshot_lower.dir().empty() ||
        !snapshot_lower.digest().empty() || !snapshot_lower.targetDigest().empty() ||
        snapshot_lower.size() != 0) {
        LOG_ERROR_RETURN(0, -1,
                         "The newest lower must reference only the current upper data file.");
    }

    auto upper = new_cfg.upper();
    if(upper.index() == conf.upper().index() || upper.data() == conf.upper().data())
        LOG_ERROR_RETURN(0, -1, "The new upper layer should be different from the old upper layer.");

    // Persist intent before mutating the live device so a lost response can
    // distinguish pre-mutation retry from post-mutation repair.
    std::string new_cfg_bytes;
    if (read_text_file(new_config_path, new_cfg_bytes) != 0) {
        LOG_ERROR_RETURN(0, -1, "Failed to read next config for restack intent.");
    }
    if (write_text_file_fsync(intent_path, new_cfg_bytes) != 0 ||
        write_text_file_fsync(phase_path, "prepared") != 0) {
        unlink(intent_path.c_str());
        unlink(phase_path.c_str());
        LOG_ERROR_RETURN(0, -1, "Failed to persist restack intent.");
    }

    upper_file = open_upper(upper);
    if (!upper_file) {
        unlink(intent_path.c_str());
        unlink(phase_path.c_str());
        LOG_ERROR_RETURN(0, -1, "Open upper layer failed.");
    }

    if(((LSMT::IFileRW *)m_file)->restack(upper_file) != 0) {
        delete upper_file;
        unlink(intent_path.c_str());
        unlink(phase_path.c_str());
        LOG_ERRNO_RETURN(0, -1, "Restack new rwlayer failed.");
    }

    // Mutation applied: further failure must not restack again.
    if (write_text_file_fsync(phase_path, "mutated") != 0) {
        LOG_ERROR_RETURN(0, -1,
                         "Restack succeeded but failed to mark journal mutated; "
                         "refusing further restacks until repaired.");
    }

    if(m_upper_file) {
        // transfer the sealed layer from m_upper_file to m_lower_file before m_upper_file is destructed
        auto sealed = ((LSMT::IFileRW *)m_upper_file)->get_file(0);
        ((LSMT::IFileRO *)m_lower_file)->insert_file(sealed);
        ((LSMT::IFileRW *)m_upper_file)->clear_files();
        safe_delete(m_upper_file);
    }
    // set m_lower_file->m_index = m_file->m_index->m_backing_index because m_file is not responsible for the destruction of m_backing_index
    auto combo_index = (LSMT::IComboIndex *)((LSMT::IFileRW *)m_file)->index(); // m_file->m_index
    ((LSMT::IFileRO *)m_lower_file)->index(combo_index->backing_index());
    // set m_file->m_index->m_index0 = upper_file->m_index
    auto upper_file_index = (LSMT::IMemoryIndex0 *)((LSMT::IFileRW *)upper_file)->index(); // upper_file->m_index
    combo_index->front_index(upper_file_index);

    m_upper_file = upper_file;

    // Prefer installing from the durable intent so retries use identical bytes.
    if (install_canonical_config(intent_path) != 0) {
        // Fall back to the caller-provided next config path if still present.
        auto lfs = photon::fs::new_localfs_adaptor();
        if (lfs == nullptr) {
            LOG_ERROR_RETURN(0, -1,
                             "Restack mutated but failed to publish canonical config; "
                             "refusing further restacks until repaired.");
        }
        DEFER(delete lfs);
        if (lfs->rename(new_config_path, this->config_path.c_str()) != 0) {
            LOG_ERROR_RETURN(0, -1,
                             "Restack mutated but failed to publish canonical config; "
                             "refusing further restacks until repaired.");
        }
    }

    conf.CopyFrom(new_cfg, conf.GetAllocator());
    unlink(phase_path.c_str());
    unlink(intent_path.c_str());
    // Caller next-config may still exist if install used intent; remove if leftover.
    unlink(new_config_path);

    return 0;
}

int ImageFile::resize(uint64_t target_size, bool resize_fs) {
    photon::scoped_rwlock lock(m_io_lock, photon::WLOCK);
    if (read_only) {
        LOG_ERROR_RETURN(EROFS, -1, "cannot resize a read-only image (no upper layer)");
    }

    auto rw = (LSMT::IFileRW *)m_file;
    struct stat st;
    if (m_file->fstat(&st) != 0) {
         LOG_ERRNO_RETURN(0, -1, "fstat failed");
    }
    uint64_t current = (uint64_t)st.st_size;

    if (target_size == current && !resize_fs) {
        LOG_INFO("vsize already equals target (` bytes), nothing to do", target_size);
        return 0;
    }

    if (target_size > current) {
        // Expand: grow block device first, then filesystem
        if (rw->update_vsize(target_size) != 0) {
            LOG_ERROR_RETURN(0, -1, "failed to update vsize for expand");
        }
    }

    if (resize_fs) {
#ifdef PHOTON_ENABLE_RESIZE
        if (photon::fs::resize_extfs(m_file, target_size, 0) != 0) {
            if (target_size >= current) {
                LOG_ERROR("resize_extfs failed. "
                          "Re-run to retry or use e2fsck to recover.");
            } else {
                LOG_ERROR("resize_extfs failed during shrink.");
            }
            return -1;
        }
#else
        LOG_ERROR_RETURN(ENOSYS, -1, "resize_fs not supported (built without PHOTON_ENABLE_RESIZE)");
#endif
    }

    if (target_size < current) {
        // Shrink: shrinking without resizing the filesystem is unsafe.
         if (!resize_fs) {
             LOG_ERROR_RETURN(EINVAL, -1, "cannot shrink without resizing filesystem");
         }
        // Shrink: update vsize after filesystem is shrunk
        if (rw->update_vsize(target_size) != 0) {
            LOG_ERROR("filesystem resized but failed to update vsize. Re-run to retry.");
            return -1;
        }
    }

    LOG_INFO("resize done: ` -> ` bytes", current, target_size);
    return 0;
}
