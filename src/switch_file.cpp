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
#include "switch_file.h"
#include <fcntl.h>
#include <photon/common/alog.h>
#include <photon/common/alog-audit.h>
#include <photon/common/alog-stdstring.h>
#include <photon/thread/thread.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/localfs.h>
#include "overlaybd/tar/tar_file.h"
#include "overlaybd/zfile/zfile.h"

using namespace std;
using namespace photon::fs;

#define FORWARD(func)                                                                              \
    if (m_local_file != nullptr) {                                                                 \
        return m_local_file->func;                                                                 \
    } else {                                                                                       \
        return m_file->func;                                                                       \
    }


// check if the `file` is zfile format
static IFile *try_open_zfile(IFile *file, bool verify, const char *file_path) {
    auto is_zfile = ZFile::is_zfile(file);
    if (is_zfile == -1) {
        LOG_ERRNO_RETURN(0, nullptr, "check file type failed.");
    }
    // open zfile
    if (is_zfile == 1) {
        auto zf = ZFile::zfile_open_ro(file, verify, true);
        if (!zf) {
            LOG_ERRNO_RETURN(0, nullptr, "zfile_open_ro failed, path: `", file_path);
        }
        LOG_INFO("open file as zfile format, path: `", file_path);
        return zf;
    }
    LOG_INFO("file is not zfile format, path: `", file_path);
    return file;
}

class SwitchFile : public ISwitchFile {
public:
    IFile *m_file = nullptr;
    IFile *m_local_file = nullptr;
    std::string m_filepath;

    SwitchFile(IFile *source, bool local = false, const char *filepath = nullptr) {
        if (local)
            m_local_file = source;
        else m_file = source;

        if (filepath != nullptr)
            m_filepath = filepath;
    };

    ~SwitchFile() {
        safe_delete(m_local_file);
        safe_delete(m_file);
    }

    void set_switch_file(const char *filepath) override {
        m_filepath = filepath;
        auto file = open_localfile_adaptor(m_filepath.c_str(), O_RDONLY, 0644, 0);
        if (file == nullptr) {
            LOG_ERROR("failed to open commit file, path: `", m_filepath);
            return;
        }
        auto tarfile = new_tar_file_adaptor(file);
        if (tarfile == nullptr) {
            delete file;
            LOG_ERROR("failed to open commit file as tar file, path: `", m_filepath);
            return;
        }
        file = tarfile;
        auto zfile = try_open_zfile(file, false, m_filepath.c_str());
        if (zfile == nullptr) {
            delete file;
            LOG_ERROR("failed to open commit file as zfile, path: `", m_filepath);
            return;
        }
        file = zfile;
        LOG_INFO("switch to localfile '`' success.", m_filepath);
        m_local_file = file;
    }

    virtual int close() override {
        return 0;
    }
    virtual ssize_t read(void *buf, size_t count) override {
        FORWARD(read(buf, count));
    }
    virtual ssize_t readv(const struct iovec *iov, int iovcnt) override {
        FORWARD(readv(iov, iovcnt));
    }
    virtual ssize_t write(const void *buf, size_t count) override {
        FORWARD(write(buf, count));
    }
    virtual ssize_t writev(const struct iovec *iov, int iovcnt) override {
        FORWARD(writev(iov, iovcnt));
    }
    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {
        if (m_local_file != nullptr) {
            SCOPE_AUDIT_THRESHOLD(10UL * 1000, "file:pread", AU_FILEOP(m_filepath, offset, count));
            return m_local_file->pread(buf, count, offset);
        } else {
            return m_file->pread(buf, count, offset);
        }
    }
    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        FORWARD(pwrite(buf, count, offset));
    }
    virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override {
        FORWARD(preadv(iov, iovcnt, offset));
    }
    virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override {
        FORWARD(pwritev(iov, iovcnt, offset));
    }
    virtual off_t lseek(off_t offset, int whence) override {
        FORWARD(lseek(offset, whence));
    }
    virtual int fstat(struct stat *buf) override {
        FORWARD(fstat(buf));
    }
    virtual IFileSystem *filesystem() override {
        FORWARD(filesystem());
    }
    virtual int fsync() override {
        FORWARD(fsync());
    }
    virtual int fdatasync() override {
        FORWARD(fdatasync());
    }
    virtual int sync_file_range(off_t offset, off_t nbytes, unsigned int flags) override {
        FORWARD(sync_file_range(offset, nbytes, flags));
    }
    virtual int fchmod(mode_t mode) override {
        FORWARD(fchmod(mode));
    }
    virtual int fchown(uid_t owner, gid_t group) override {
        FORWARD(fchown(owner, group));
    }
    virtual int ftruncate(off_t length) override {
        FORWARD(ftruncate(length));
    }
    virtual int fallocate(int mode, off_t offset, off_t len) override {
        FORWARD(fallocate(mode, offset, len));
    }
};

ISwitchFile *new_switch_file(IFile *source, bool local, const char *file_path) {
    int retry = 1;
again:
    auto file = try_open_zfile(source, !local, file_path);
    if (file == nullptr) {
        LOG_ERROR("failed to open source file as zfile, path: `, retry: `", file_path, retry);
        if (retry--) // may retry after cache evict
            goto again;
        return nullptr;
    }
    return new SwitchFile(file, local, file_path);
};
