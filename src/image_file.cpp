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
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/aligned-file.h>
#include <photon/fs/localfs.h>
#include <photon/fs/path.h>
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

static const char *SNAPSHOT_SUFFIX = ".snapshot";
static const char *SNAPSHOT_UNDO_SUFFIX = ".snapshot.undo";

static int drop_snapshot_journals(const std::string &config_path) {
    int ret = 0;
    for (auto suffix : {SNAPSHOT_SUFFIX, SNAPSHOT_UNDO_SUFFIX}) {
        auto path = config_path + suffix;
        if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
            LOG_ERROR("unlink(`) failed: `", path, strerror(errno));
            ret = -1;
        }
    }
    return ret;
}

static int write_file(const std::string &path, const std::string &content) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        LOG_ERRNO_RETURN(0, -1, "open(`) failed", path);
    DEFER(::close(fd));
    auto p = content.data();
    size_t left = content.size();
    while (left > 0) {
        auto ret = ::write(fd, p, left);
        if (ret <= 0) {
            if (ret < 0 && errno == EINTR)
                continue;
            LOG_ERRNO_RETURN(0, -1, "write(`) failed", path);
        }
        p += ret;
        left -= ret;
    }
    return 0;
}

static int read_file_content(const std::string &path, std::string &content) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        LOG_ERRNO_RETURN(0, -1, "open(`) failed", path);
    DEFER(::close(fd));
    char buf[4096];
    while (true) {
        auto ret = ::read(fd, buf, sizeof(buf));
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            LOG_ERRNO_RETURN(0, -1, "read(`) failed", path);
        }
        if (ret == 0)
            break;
        content.append(buf, ret);
    }
    return 0;
}

static std::string absolute_path(const std::string &path) {
    if (!path.empty() && path[0] == '/')
        return path;
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf)) == nullptr)
        return path;
    return std::string(buf) + "/" + path;
}

static int mkdir_p(const std::string &path) {
    auto lfs = new_localfs_adaptor();
    if (lfs == nullptr)
        LOG_ERRNO_RETURN(0, -1, "new localfs adaptor failed");
    DEFER(delete lfs);
    // the trailing slash makes photon create the last component as well
    return photon::fs::mkdir_recursive(path + "/", lfs);
}

// the record of a move: where the layer is and where it was moved to
static std::string snapshot_move_record(const std::string &data_path, const std::string &move_to) {
    return absolute_path(data_path) + "\n" + move_to + "\n";
}

static int parse_snapshot_move_record(const std::string &record, std::string &data_path,
                                      std::string &move_to) {
    auto path_end = record.find('\n');
    if (path_end == std::string::npos || path_end == 0)
        return -1;
    auto move_end = record.find('\n', path_end + 1);
    if (move_end == std::string::npos)
        return -1;
    data_path = record.substr(0, path_end);
    move_to = record.substr(path_end + 1, move_end - path_end - 1);
    if (data_path[0] != '/' || move_to.empty() || move_to[0] != '/')
        return -1;
    return 0;
}

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

static int rename_noreplace(const std::string &from, const std::string &to) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 28))
    return ::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE);
#elif defined(SYS_renameat2)
    return ::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(),
                     RENAME_NOREPLACE);
#else
    errno = ENOSYS;
    return -1;
#endif
}

// moves the layer without ever replacing an entry which is already there
static int move_layer(const std::string &from, const std::string &to) {
    if (rename_noreplace(from, to) != 0)
        LOG_ERRNO_RETURN(0, -1, "rename(`) to ` failed", from, to);
    return 0;
}

// puts the layer back where it was; the original name is gone only after a move
// that succeeded, so its presence means there is nothing to move back
static int move_layer_back(const std::string &data_path, const std::string &move_to) {
    if (::access(data_path.c_str(), F_OK) == 0)
        return 0;
    if (::access(move_to.c_str(), F_OK) != 0)
        LOG_ERROR_RETURN(ENOENT, -1, "` is gone", data_path);
    return move_layer(move_to, data_path);
}

static bool layer_config_equal(ImageConfigNS::LayerConfig &a, ImageConfigNS::LayerConfig &b) {
    return a.file() == b.file() && a.gzipIndex() == b.gzipIndex() &&
           a.targetFile() == b.targetFile() && a.dir() == b.dir() &&
           a.digest() == b.digest() && a.targetDigest() == b.targetDigest() &&
           a.size() == b.size();
}

// a retry after a lost response describes exactly the committed stack
static bool snapshot_request_matches(ImageConfigNS::ImageConfig &committed,
                                     ImageConfigNS::ImageConfig &request) {
    auto committed_lowers = committed.lowers();
    auto request_lowers = request.lowers();
    if (committed_lowers.size() != request_lowers.size())
        return false;
    for (size_t i = 0; i < committed_lowers.size(); i++) {
        if (!layer_config_equal(committed_lowers[i], request_lowers[i]))
            return false;
    }
    auto committed_upper = committed.upper();
    auto request_upper = request.upper();
    return committed_upper.index() == request_upper.index() &&
           committed_upper.data() == request_upper.data() &&
           committed_upper.target() == request_upper.target() &&
           committed_upper.gzipIndex() == request_upper.gzipIndex();
}

static bool same_dir_entry(const std::string &a, const std::string &b) {
    auto a_parts = photon::fs::Path(absolute_path(a)).dir_base_name();
    auto b_parts = photon::fs::Path(absolute_path(b)).dir_base_name();
    if (a_parts.second != b_parts.second)
        return false;
    char a_real[PATH_MAX], b_real[PATH_MAX];
    if (::realpath(std::string(a_parts.first).c_str(), a_real) == nullptr ||
        ::realpath(std::string(b_parts.first).c_str(), b_real) == nullptr)
        return false;
    return std::string(a_real) == b_real;
}

// a layer which occupies a transaction file name would be deleted by the recovery,
// so such a config is refused before anything is touched
static int reject_journal_collision(ImageConfigNS::ImageConfig &cfg,
                                    const std::string &config_path) {
    const std::string records[] = {config_path + SNAPSHOT_SUFFIX,
                                   config_path + SNAPSHOT_UNDO_SUFFIX};
    auto upper = cfg.upper();
    auto lowers = cfg.lowers();
    std::vector<std::string> layers = {upper.data(), upper.index()};
    for (size_t i = 0; i < lowers.size(); i++)
        layers.push_back(lowers[i].file());
    for (size_t i = 0; i < layers.size(); i++) {
        if (layers[i].empty())
            continue;
        for (auto &record : records) {
            if (same_dir_entry(layers[i], record))
                LOG_ERROR_RETURN(EINVAL, -1,
                                 "the layer ` collides with a snapshot record of `, rename it.",
                                 layers[i], config_path);
        }
    }
    return 0;
}

// the sealed layer is moved when the request names another path for it, so the
// target must be free and reachable by rename
static int snapshot_move_target(ImageConfigNS::ImageConfig &committed,
                                ImageConfigNS::LayerConfig &appended, std::string &move_to) {
    move_to.clear();
    auto data_path = committed.upper().data();
    if (appended.file() == data_path)
        return 0;

    struct stat target, data, parent;
    if (::lstat(appended.file().c_str(), &target) == 0)
        LOG_ERROR_RETURN(EEXIST, -1, "the snapshot target ` already exists.", appended.file());
    if (errno != ENOENT)
        LOG_ERRNO_RETURN(0, -1, "stat(`) failed", appended.file());
    move_to = absolute_path(appended.file());
    // the undo record is line based, so a path holding a newline can not be stored
    if (data_path.find('\n') != std::string::npos || move_to.find('\n') != std::string::npos)
        LOG_ERROR_RETURN(EINVAL, -1, "` or ` holds a newline, which the move record can not store.",
                         data_path, move_to);
    auto dir = move_to.substr(0, move_to.find_last_of('/'));
    if (dir.empty())
        dir = "/";
    if (mkdir_p(dir) != 0)
        LOG_ERRNO_RETURN(0, -1, "failed to create the directory of `", move_to);
    if (::stat(dir.c_str(), &parent) != 0)
        LOG_ERRNO_RETURN(0, -1, "stat(`) failed", dir);
    if (!S_ISDIR(parent.st_mode))
        LOG_ERROR_RETURN(ENOTDIR, -1, "` is not a directory.", dir);
    if (::lstat(data_path.c_str(), &data) != 0)
        LOG_ERRNO_RETURN(0, -1, "lstat(`) failed", data_path);
    // a rename moves the link itself, which would leave the new lower dangling
    if (S_ISLNK(data.st_mode))
        LOG_ERROR_RETURN(EINVAL, -1, "the upper data ` is a symlink, refusing to move it.",
                         data_path);
    if (parent.st_dev != data.st_dev)
        LOG_ERROR_RETURN(EXDEV, -1, "` is not on the same file system as `.", move_to, data_path);
    return 0;
}

static int validate_snapshot_config(ImageConfigNS::ImageConfig &committed,
                                    ImageConfigNS::ImageConfig &request, std::string &move_to) {
    auto committed_upper = committed.upper();
    auto request_upper = request.upper();
    if (committed_upper.target() != "" || request_upper.target() != "")
        LOG_ERROR_RETURN(ENOTSUP, -1, "snapshot is not supported with a warpfile upper layer");
    if (request_upper.index() == committed_upper.index() ||
        request_upper.data() == committed_upper.data())
        LOG_ERROR_RETURN(0, -1,
                         "The new upper layer(`, `) should be different from the old upper layer(`, `).",
                         request_upper.data(), request_upper.index(), committed_upper.data(),
                         committed_upper.index());

    auto committed_lowers = committed.lowers();
    auto request_lowers = request.lowers();
    if (request_lowers.size() != committed_lowers.size() + 1)
        LOG_ERROR_RETURN(0, -1, "new lowers count is `, expect ` (the old lowers plus the old upper).",
                         request_lowers.size(), committed_lowers.size() + 1);
    for (size_t i = 0; i < committed_lowers.size(); i++) {
        if (!layer_config_equal(committed_lowers[i], request_lowers[i]))
            LOG_ERROR_RETURN(0, -1, "the `-th lower of the new config differs from the current one.",
                             i);
    }
    auto &appended = request_lowers.back();
    if (appended.gzipIndex() != "" || appended.targetFile() != "" || appended.dir() != "" ||
        appended.digest() != "" || appended.targetDigest() != "" || appended.size() != 0)
        LOG_ERROR_RETURN(0, -1,
                         "the last lower(`) should only reference the old upper data(`), got dir(`) digest(`) size(`).",
                         appended.file(), committed_upper.data(), appended.dir(),
                         appended.digest(), appended.size());
    return snapshot_move_target(committed, appended, move_to);
}

IFile *ImageFile::__open_ro_file(const std::string &path) {
    int flags = O_RDONLY;

    LOG_INFO("open ro file: `", path);
    auto ioengine = image_service.global_conf.ioEngine();
    if (ioengine > ImageConfigNS::IOENGINE_IOURING_DIRECT) {
        LOG_WARN("invalid ioengine: `, set to psync", ioengine);
        ioengine = 0;
    }
    bool direct = ioengine == ioengine_libaio ||
                  ioengine == ImageConfigNS::IOENGINE_IOURING_DIRECT;
    if (ImageConfigNS::is_iouring_ioengine(ioengine))
        ioengine = ioengine_iouring;
    if (direct) {
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
    const std::string candidates[] = {
        layer.file(),
        layer.dir() + "/" + COMMIT_FILE_NAME,
        layer.dir() + "/" + LEGACY_COMMIT_FILE_NAME,
        layer.dir() + "/" + SEALED_FILE_NAME
    };

    for (const auto &path : candidates) {
        if (::access(path.c_str(), F_OK) == 0) {
            opened = path;
            return __open_ro_file(opened);
        }
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
    auto engine = image_service.global_conf.ioEngine();
    if (!upper.target().empty() || engine == ioengine_posixaio)
        engine = ioengine_psync;
    bool direct = engine == ioengine_libaio ||
                  engine == ImageConfigNS::IOENGINE_IOURING_DIRECT;
    if (ImageConfigNS::is_iouring_ioengine(engine))
        engine = ioengine_iouring;
    data_file = open_localfile_adaptor(upper.data().c_str(),
                                       O_RDWR | (direct ? O_DIRECT : 0), 0644, engine);
    if (!data_file) {
        LOG_ERROR("open(`,flags), `:`", upper.data(), errno, strerror(errno));
        goto ERROR_EXIT;
    }
    if (direct) {
        auto aligned = new_aligned_file_adaptor(data_file, ALIGNMENT_4K, true, true);
        if (!aligned)
            goto ERROR_EXIT;
        data_file = aligned;
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
    if (ret->set_data_file_direct_io(direct) != 0) {
        delete ret;
        return nullptr;
    }
    return ret;

ERROR_EXIT:
    delete data_file;
    delete idx_file;
    delete ret;
    return NULL;
}

int ImageFile::init_image_file() {
    // a pending snapshot must be rolled back before any layer is opened
    if (recover_pending_snapshot() != 0) {
        set_failed("failed to recover the pending snapshot");
        return -1;
    }

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
        m_prefetcher->replay(this);
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

template <typename... Ts>
void ImageFile::set_failed(const Ts &...xs) {
    if (m_status == 0) // only set exit in image boot phase
    {
        m_status = -1;
        m_exception = estring().appends(xs...);
    }
}

int ImageFile::recover_pending_snapshot() {
    if (reject_journal_collision(conf, config_path) != 0)
        return -1;

    auto pending_path = config_path + SNAPSHOT_SUFFIX;
    auto undo_path = config_path + SNAPSHOT_UNDO_SUFFIX;
    if (::access(pending_path.c_str(), F_OK) != 0) {
        // the snapshot is either committed or never started, so nothing on disk
        // needs to be undone
        if (::access(undo_path.c_str(), F_OK) == 0 && drop_snapshot_journals(config_path) != 0)
            LOG_ERROR_RETURN(0, -1, "failed to remove the snapshot records, refusing to start.");
        return 0;
    }

    // the config is not updated: put a moved layer back
    if (::access(undo_path.c_str(), F_OK) == 0) {
        std::string record, data_path, move_to;
        if (read_file_content(undo_path, record) != 0 ||
            parse_snapshot_move_record(record, data_path, move_to) != 0 ||
            data_path != absolute_path(conf.upper().data()))
            LOG_ERROR_RETURN(EINVAL, -1, "` is invalid, refusing to start.", undo_path);
        if (move_layer_back(data_path, move_to) != 0)
            LOG_ERROR_RETURN(0, -1, "move ` back to ` failed, refusing to start.", move_to,
                             data_path);
    }
    if (drop_snapshot_journals(config_path) != 0)
        LOG_ERROR_RETURN(0, -1, "failed to remove the snapshot records, refusing to start.");
    LOG_INFO("the uncommitted snapshot of ` is discarded.", config_path);
    return 0;
}

void ImageFile::swap_snapshot_layers(LSMT::IFileRW *upper_file) {
    if(m_upper_file) {
        // transfer the sealed layer from m_upper_file to m_lower_file before m_upper_file is destructed
        auto sealed = ((LSMT::IFileRW *)m_upper_file)->get_file(0);
        ((LSMT::IFileRO *)m_lower_file)->insert_file(sealed);
        ((LSMT::IFileRW *)m_upper_file)->clear_files();
        safe_delete(m_upper_file);
    }
    // m_file already writes into upper_file, only the ownership moves here
    auto combo_index = (LSMT::IComboIndex *)((LSMT::IFileRW *)m_file)->index(); // m_file->m_index
    ((LSMT::IFileRO *)m_lower_file)->index(combo_index->backing_index());

    m_upper_file = upper_file;
}

int ImageFile::create_snapshot(const char *new_config_path) {
    // load new config file to get the snapshot layer path
    // open new upper layer
    // restack() current RW layer as snapshot layer
    if(!m_lower_file || !m_upper_file)
        LOG_ERROR_RETURN(0, -1, "Lower or upper layer is NULL.");

    typedef photon::scoped_lock Lock;
    Lock lock(m_snapshot_mtx);

    auto pending_path = config_path + SNAPSHOT_SUFFIX;
    auto undo_path = config_path + SNAPSHOT_UNDO_SUFFIX;
    bool pending = ::access(pending_path.c_str(), F_OK) == 0;
    bool undo = ::access(undo_path.c_str(), F_OK) == 0;
    // only both records together mean that a move may still be in flight
    if (pending && undo)
        LOG_ERROR_RETURN(EBUSY, -1,
                         "a previous snapshot is not committed, restart the device to recover "
                         "before taking another one.");
    if ((pending || undo) && drop_snapshot_journals(config_path) != 0)
        LOG_ERROR_RETURN(0, -1, "failed to remove the stale snapshot records of `", config_path);

    std::string new_cfg_bytes;
    ImageConfigNS::ImageConfig new_cfg;
    LOG_INFO("Load new config `.", new_config_path);
    if (read_file_content(new_config_path, new_cfg_bytes) != 0)
        return -1;
    if (!new_cfg.ParseJSONStream(new_cfg_bytes)) {
        LOG_ERROR_RETURN(0, -1, "Error parse new config json: `.", new_config_path);
    }
    if (reject_journal_collision(new_cfg, config_path) != 0)
        return -1;

    if (snapshot_request_matches(conf, new_cfg)) {
        LOG_INFO("snapshot of ` is already committed, nothing to do.", new_config_path);
        return 0;
    }
    std::string move_to;
    if (validate_snapshot_config(conf, new_cfg, move_to) != 0)
        return -1;

    auto upper = new_cfg.upper();
    std::unique_ptr<LSMT::IFileRW> upper_guard(open_upper(upper));
    auto upper_file = upper_guard.get();
    if (!upper_file)
        LOG_ERROR_RETURN(0, -1, "Open upper layer failed.");

    if (((LSMT::IFileRO *)m_lower_file)->reserve_files(1) != 0)
        LOG_ERROR_RETURN(0, -1, "failed to reserve a slot for the sealed layer.");

    auto data_path = absolute_path(conf.upper().data());

    // the move record comes first, so a pending `.snapshot` always has a complete one
    if (!move_to.empty() && write_file(undo_path, snapshot_move_record(data_path, move_to)) != 0) {
        drop_snapshot_journals(config_path);
        return -1;
    }
    if (write_file(pending_path, new_cfg_bytes) != 0) {
        drop_snapshot_journals(config_path);
        return -1;
    }

    auto commit = [&]() -> int {
        if (!move_to.empty() && move_layer(data_path, move_to) != 0)
            return -1;
        if (::rename(pending_path.c_str(), config_path.c_str()) != 0) {
            int rename_errno = errno;
            if (!move_to.empty() && move_layer_back(data_path, move_to) != 0)
                LOG_ERROR_RETURN(0, -1, "the layer stays at `", move_to);
            errno = rename_errno;
            LOG_ERRNO_RETURN(0, -1, "rename(`) to ` failed", pending_path, config_path);
        }
        return 0;
    };

    if (((LSMT::IFileRW *)m_file)->restack(upper_file, commit) != 0) {
        int saved_errno = errno;
        // the records are kept only when the layer can not be put back, so that
        // the next start can finish it
        if (!move_to.empty() && move_layer_back(data_path, move_to) != 0)
            LOG_ERROR_RETURN(0, -1, "move ` back to ` failed, keep the records.", move_to,
                             data_path);
        if (drop_snapshot_journals(config_path) != 0)
            LOG_WARN("failed to remove the snapshot records of `", config_path);
        errno = saved_errno;
        LOG_ERRNO_RETURN(0, -1, "Restack new rwlayer failed.");
    }

    swap_snapshot_layers(upper_file);
    upper_guard.release();
    conf.CopyFrom(new_cfg, conf.GetAllocator());
    ::unlink(undo_path.c_str());
    LOG_INFO("snapshot committed, config ` is updated.", config_path);
    return 0;
}

int ImageFile::resize(uint64_t target_size, bool resize_fs) {
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
