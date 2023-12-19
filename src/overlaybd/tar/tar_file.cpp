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
#include "tar_file.h"

#include <photon/common/alog.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/forwardfs.h>
#include <photon/common/estring.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/param.h>
#include <sys/types.h>
#include <stdlib.h>
#include "libtar.h"

using namespace std;
using namespace photon::fs;

#define NO_TIMESTAMP 1

#define TMAGIC_EMPTY "xxtar"
#define TVERSION_EMPTY "xx"

// forward declaration
IFile *new_tar_file(IFile *file, bool create = false);
IFile *open_tar_file(IFile *file);

size_t strlcpy(char *dst, char *src, size_t siz) {
    char *d = dst;
    const char *s = src;
    size_t n = siz;

    /* Copy as many bytes as will fit */
    if (n != 0 && --n != 0) {
        do {
            if ((*d++ = *s++) == 0)
                break;
        } while (--n != 0);
    }

    /* Not enough room in dst, add NUL and traverse rest of src */
    if (n == 0) {
        if (siz != 0)
            *d = '\0'; /* NUL-terminate dst */
        while (*s++)
            ;
    }

    return (s - src - 1); /* count does not include NUL */
}

class TarFile : public ForwardFile_Ownership {
public:
    TarFile(IFile *file, bool create = false) : ForwardFile_Ownership(file, true) {
        if (create) {
            if (!mark_new_tar()) {
                LOG_ERROR("mark new tar failed");
                return;
            }
        }
        m_tar = new TarCore(file, TAR_IGNORE_CRC);
    }

    ~TarFile() {
        close();
        delete m_tar;
    }

    size_t read_header() {
        if (!m_tar || m_tar->read_header() != 0) {
            LOG_ERRNO_RETURN(0, -1, "read tar header failed.");
        }
        m_size = m_tar->get_size();
        base_offset = T_BLOCKSIZE;
        if (m_tar->has_pax_header()) {
            base_offset = 3 * T_BLOCKSIZE;
        }
        m_file->lseek(base_offset, SEEK_SET);   // notice: lseek does not work for CachedFile
        LOG_INFO(VALUE(m_size), VALUE(base_offset));
        return 0;
    }

    virtual int fstat(struct stat *buf) override {
        int ret = m_file->fstat(buf);
        if (ret < 0) {
            return ret;
        }
        if (is_new_tar()) {
            buf->st_size -= base_offset;
        } else {
            buf->st_size = m_size;
        }
        return ret;
    }

    virtual off_t lseek(off_t offset, int whence) override {
        off_t ret = -1;
        switch (whence) {
        case SEEK_SET:
            offset += base_offset;
            ret = m_file->lseek(offset, SEEK_SET);
            break;

        case SEEK_CUR:
            ret = m_file->lseek(offset, SEEK_CUR);
            break;

        case SEEK_END:
            if (is_new_tar()) {
                ret = m_file->lseek(offset, SEEK_END);
            } else {
                offset += m_tar->get_size() + base_offset;
                ret = m_file->lseek(offset, SEEK_SET);
            }
            break;

        default:
            errno = EINVAL;
            LOG_ERROR(VALUE(errno));
            return -1;
        }

        if (ret >= 0) {
            return ret - base_offset;
        } else {
            errno = EINVAL;
            return -1;
        }
    }

    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {
        return m_file->pread(buf, count, offset + base_offset);
    }
    virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override {
        return m_file->preadv(iov, iovcnt, offset + base_offset);
    }
    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        return m_file->pwrite(buf, count, offset + base_offset);
    }
    virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override {
        return m_file->pwritev(iov, iovcnt, offset + base_offset);
    }
    virtual int fallocate(int mode, off_t offset, off_t len) override {
        return m_file->fallocate(mode, offset + base_offset, len);
    }
    virtual int fadvise(off_t offset, off_t len, int advice) override {
        return m_file->fadvise(offset + base_offset, len, advice);
    }

    virtual int close() override {
        if (is_new_tar()) {
            LOG_INFO("write header for tar file");
            write_header_trailer();
        }
        return m_file->close();
    }

private:
    TarCore *m_tar = nullptr;
    off_t base_offset;
    size_t m_size;
    int write_header_trailer() {
        struct stat s;
        m_file->fstat(&s);

        char buf[3 * T_BLOCKSIZE];
        memset(buf, 0, 3 * T_BLOCKSIZE);
        // pax header, 2 blocks
        auto th_buf = (TarHeader *)buf;
        th_buf->typeflag = PAX_HEADER;                                  // type
        snprintf(th_buf->name, 100, "%.100s", "overlaybd.pax");         // name
        auto size = s.st_size - 3 * T_BLOCKSIZE;
        auto record = format_pax_record("size", to_string(size));
        LOG_DEBUG(VALUE(record.c_str()), VALUE(record.size()));
        int_to_oct_nonull(record.size(), th_buf->size, 12);             // size
        strncpy(th_buf->version, TVERSION, TVERSLEN);                   // version
        strncpy(th_buf->magic, TMAGIC, TMAGLEN);                        // magic
        int_to_oct(th_buf->crc_calc(), th_buf->chksum, 8);              // checksum
        memcpy(buf + T_BLOCKSIZE, record.c_str(), record.size());
        // tar header, 1 block
        th_buf = (TarHeader *)(buf + 2 * T_BLOCKSIZE);
        th_buf->typeflag = REGTYPE;                                     // type
        struct passwd *pw;
        pw = getpwuid(0);
        if (pw != NULL)
            strlcpy(th_buf->uname, pw->pw_name, sizeof(th_buf->uname)); // uname
        int_to_oct(0, th_buf->uid, 8);                                  // uid
        struct group *gr;
        gr = getgrgid(0);
        if (gr != NULL)
            strlcpy(th_buf->gname, gr->gr_name, sizeof(th_buf->gname)); // gname
        int_to_oct(0, th_buf->gid, 8);                                  // gid
        int_to_oct(s.st_mode, th_buf->mode, 8);                         // mode
#ifndef NO_TIMESTAMP
        int_to_oct_nonull(s.st_mtime, th_buf->mtime, 12);               // mtime
#else
        int_to_oct_nonull(0, th_buf->mtime, 12);
#endif
        int_to_oct_nonull(0, th_buf->size, 12);                         // size
        snprintf(th_buf->name, 100, "%.100s", "overlaybd.commit");      // name
        strncpy(th_buf->version, TVERSION, TVERSLEN);                   // version
        strncpy(th_buf->magic, TMAGIC, TMAGLEN);                        // magic
        int_to_oct(th_buf->crc_calc(), th_buf->chksum, 8);              // checksum
        // write header
        m_file->pwrite(buf, 3 * T_BLOCKSIZE, 0);
        // write trailer
        memset(th_buf, 0, T_BLOCKSIZE);
        m_file->pwrite(th_buf, T_BLOCKSIZE,
                       (s.st_size + 511) / T_BLOCKSIZE * T_BLOCKSIZE);
        m_file->pwrite(th_buf, T_BLOCKSIZE,
                       (s.st_size + 511) / T_BLOCKSIZE * T_BLOCKSIZE + T_BLOCKSIZE);
        return 0;
    }

    bool mark_new_tar() {
        LOG_INFO("new tar header");
        char buf[3 * T_BLOCKSIZE];
        memset(buf, 0, 3 * T_BLOCKSIZE);
        // pax header, 2 blocks
        auto th_buf = (TarHeader *)buf;
        th_buf->typeflag = PAX_HEADER;                          // type
        snprintf(th_buf->name, 100, "%.100s", "overlaybd.pax"); // name
        auto record = format_pax_record("size", "0");
        LOG_DEBUG(VALUE(record.c_str()), VALUE(record.size()));
        int_to_oct_nonull(record.size(), th_buf->size, 12);     // size
        memcpy(buf + T_BLOCKSIZE, record.c_str(), record.size());
        // tar header, 1 block
        th_buf = (TarHeader *)(buf + 2 * T_BLOCKSIZE);
        snprintf(th_buf->name, 100, "%.100s", "overlaybd.new"); // name
        strncpy(th_buf->version, TVERSION_EMPTY, TVERSLEN);     // version
        strncpy(th_buf->magic, TMAGIC_EMPTY, TMAGLEN);          // magic
        int_to_oct_nonull(0, th_buf->size, 12);                 // size
        // write header
        return (m_file->pwrite(buf, 3 * T_BLOCKSIZE, 0) == 3 * T_BLOCKSIZE);
    }

    bool is_new_tar() {
        if (m_tar &&
            strncmp(m_tar->header.magic, TMAGIC_EMPTY, TMAGLEN - 1) == 0 &&
            strncmp(m_tar->header.version, TVERSION_EMPTY, TVERSLEN) == 0) {
            return true;
        }
        return false;
    }

    std::string format_pax_record(const std::string &key, const std::string &value) {
        size_t size = key.length() + value.length() + 3; // padding for ' ', '=' and '\n'
        size += to_string(size).length();
        std::string record = to_string(size) + " " + key + "=" + value + "\n";
        if (record.length() != size) {
            size = record.length();
            record = to_string(size) + " " + key + "=" + value + "\n";
        }
        return record;
    }
}; // namespace FileSystem

class TarFs : public ForwardFS_Ownership {
public:
    TarFs(IFileSystem *fs) : ForwardFS_Ownership(fs, true) {
    }

    IFile *open(const char *pathname, int flags, mode_t mode) {
        IFile *file = m_fs->open(pathname, flags, mode);
        return open_tar(file, flags);
    }
    IFile *open(const char *pathname, int flags) {
        IFile *file = m_fs->open(pathname, flags);
        return open_tar(file, flags);
    }

private:
    IFile *open_tar(IFile *file, int flags) {
        if (!file) {
            return nullptr;
        }
        if (flags & O_RDONLY) {
            return open_tar_file(file);
        }
        struct stat s;
        file->fstat(&s);
        if (s.st_size == 0) {
            return new_tar_file(file, true);
        }
        return open_tar_file(file);
    }
};


int is_tar_file(IFile *file) {
    TarHeader th_buf;
    auto ret = file->pread(&th_buf, T_BLOCKSIZE, 0);
    if (ret < 0) {
        LOG_ERRNO_RETURN(0, -1, "read tar file header failed");
    } else if (ret != T_BLOCKSIZE) {
        LOG_WARN("read tar file header error, expect `, ret `", T_BLOCKSIZE, ret);
        return 0;
    }
    if (strncmp(th_buf.magic, TMAGIC, TMAGLEN - 1) != 0) {
        LOG_INFO("unknown magic value in tar header");
        return 0;
    }
    if (strncmp(th_buf.version, TVERSION, TVERSLEN) != 0) {
        LOG_INFO("unknown version value in tar header");
        return 0;
    }
    if (!th_buf.crc_ok()) {
        LOG_INFO("tar header checksum error");
        return 0;
    }
    return 1;
}

IFile *new_tar_file(IFile *file, bool create) {
    auto ret = new TarFile(file, create);
    if (ret->read_header() != 0) {
        LOG_ERRNO_RETURN(0, nullptr, "read tar header failed.");
    }
    return ret;
}

IFile *open_tar_file(IFile *file) {
    if (!file) {
        LOG_ERROR_RETURN(0, nullptr, "file is nullptr");
    }
    auto ret = is_tar_file(file);
    if (ret == 1) {
        LOG_INFO("open file as tar file");
        return new_tar_file(file);
    } else if (ret == 0) {
        LOG_INFO("open file as normal file");
        return file;
    } else {
        LOG_ERROR_RETURN(0, nullptr, "open tar file failed");
    }
}

IFileSystem *new_tar_fs_adaptor(IFileSystem *fs) {
    return new TarFs(fs);
}

IFile *new_tar_file_adaptor(IFile *file) {
    return open_tar_file(file);
}
