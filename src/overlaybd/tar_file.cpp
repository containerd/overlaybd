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
#include "untar/libtar.h"

using namespace std;
using namespace photon::fs;

#define NO_TIMESTAMP 1

/* Contents of magic field and its length.  */
#define TMAGIC "ustar"
#define TMAGLEN 6

/* Contents of the version field and its length.  */
#define TVERSION "00"
#define TVERSLEN 2

#define TMAGIC_EMPTY "xxtar"
#define TVERSION_EMPTY "xx"

#define TAR_HEADER_SIZE 512

#define int_to_oct(num, oct, octlen)                                                               \
    snprintf((oct), (octlen), "%*lo ", (octlen)-2, (unsigned long)(num))

// forward declaration
static IFile *open_tar_file(IFile *file, bool verify_type = true);

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};

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

int th_crc_calc(struct tar_header &th_buf) {
    int i, sum = 0;
    for (i = 0; i < TAR_HEADER_SIZE; i++)
        sum += ((unsigned char *)(&(th_buf)))[i];
    for (i = 0; i < 8; i++)
        sum += (' ' - (unsigned char)th_buf.chksum[i]);

    return sum;
}
int th_signed_crc_calc(struct tar_header &th_buf) {
    int i, sum = 0;

    for (i = 0; i < TAR_HEADER_SIZE; i++)
        sum += ((signed char *)(&(th_buf)))[i];
    for (i = 0; i < 8; i++)
        sum += (' ' - (signed char)th_buf.chksum[i]);

    return sum;
}

class TarFile : public ForwardFile_Ownership {
public:
    TarFile(IFile *file) : ForwardFile_Ownership(file, true) {
    }

    ~TarFile() {
        close();
    }

    size_t read_header() {

        struct tar_header th_buf;
        memset(&(th_buf), 0, sizeof(struct tar_header));
        m_file->pread(&th_buf, TAR_HEADER_SIZE, 0);
        base_offset = TAR_HEADER_SIZE;
        if (th_buf.typeflag == 'x') {
            base_offset = 3 * TAR_HEADER_SIZE;
            auto size = oct_to_size(th_buf.size);
            LOG_DEBUG("read PAX extended header. (size: `B)", size);
            assert(size < TAR_HEADER_SIZE);
            char buffer[TAR_HEADER_SIZE]{};
            m_file->pread(buffer, size, TAR_HEADER_SIZE);
            int prev = 0;
            for (off_t i = 0; i < size; i++) {
                if (buffer[i] == '\n') {
                    estring_view attr(buffer + prev, buffer + i);
                    if (attr.find("size=") != string::npos) {
                        auto p = attr.find("size=") + strlen("size=");
                        auto str_size = attr.substr(p);
                        m_size = strtoll(str_size.to_string().c_str(), nullptr, 10);
                        if (m_size == 0) {
                            LOG_ERRNO_RETURN(0, -1, "get file size error.");
                        }
                        LOG_DEBUG("file size: `", m_size);
                        break;
                    }
                    prev = i + 1;
                }
            }
        } else {
            m_size = oct_to_size(th_buf.size);
        }
        m_file->lseek(base_offset, SEEK_SET);
        return 0;
    }

    virtual int fstat(struct stat *buf) override {

        int ret = m_file->fstat(buf);
        if (ret < 0) {
            return ret;
        }
        buf->st_size = m_size;
        return ret;
    }

    virtual off_t lseek(off_t offset, int whence) override {
        off_t ret = -1;
        size_t size = -1;
        switch (whence) {
        case SEEK_SET:
            offset += base_offset;
            ret = m_file->lseek(offset, SEEK_SET);
            break;

        case SEEK_CUR:
            ret = m_file->lseek(offset, SEEK_CUR);
            break;

        case SEEK_END:
            struct tar_header th_buf;
            memset(&(th_buf), 0, sizeof(struct tar_header));
            m_file->pread(&th_buf, TAR_HEADER_SIZE, 0);
            size = oct_to_size(th_buf.size);
            if (size == -1)
                ret = m_file->lseek(offset, SEEK_END);
            else
                ret = m_file->lseek(size + offset, SEEK_SET);
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

    virtual int close() override {
        struct stat s;
        m_file->fstat(&s);
        struct tar_header th_buf;
        memset(&(th_buf), 0, sizeof(struct tar_header));
        m_file->pread(&th_buf, TAR_HEADER_SIZE, 0);
        if (strncmp(th_buf.magic, TMAGIC_EMPTY, TMAGLEN - 1) == 0 &&
            strncmp(th_buf.version, TVERSION_EMPTY, TVERSLEN) == 0) {
            LOG_INFO("write header for tar file");
            write_header_trailer();
        }
        return m_file->close();
    }

private:
    off_t base_offset;
    size_t m_size;
    int write_header_trailer() {
        struct stat s;
        m_file->fstat(&s);
        struct tar_header th_buf;
        memset(&(th_buf), 0, sizeof(struct tar_header));
        // th_set_type
        th_buf.typeflag = '0';
        // th_set_user
        struct passwd *pw;
        pw = getpwuid(s.st_uid);
        if (pw != NULL)
            strlcpy(th_buf.uname, pw->pw_name, sizeof(th_buf.uname));
        int_to_oct(s.st_uid, th_buf.uid, 8);
        // th_set_group
        struct group *gr;
        gr = getgrgid(s.st_gid);
        if (gr != NULL)
            strlcpy(th_buf.gname, gr->gr_name, sizeof(th_buf.gname));
        int_to_oct(s.st_gid, th_buf.gid, 8);
        // th_set_mode
        int_to_oct(s.st_mode, th_buf.mode, 8);
        // th_set_mtime
#ifndef NO_TIMESTAMP
        int_to_oct_nonull(s.st_mtime, th_buf.mtime, 12);
#else
        int_to_oct_nonull(0, th_buf.mtime, 12);
#endif
        // th_set_size
        int_to_oct_nonull(s.st_size - TAR_HEADER_SIZE, th_buf.size, 12);
        // th_set_path
        snprintf(th_buf.name, 100, "%s", "overlaybd.commit");

        /* magic, version, and checksum */
        strncpy(th_buf.version, TVERSION, TVERSLEN);
        strncpy(th_buf.magic, TMAGIC, TMAGLEN);
        int_to_oct(th_crc_calc(th_buf), th_buf.chksum, 8);

        m_file->pwrite(&th_buf, TAR_HEADER_SIZE, 0);

        memset(&(th_buf), 0, sizeof(struct tar_header));
        m_file->pwrite(&th_buf, TAR_HEADER_SIZE,
                       (s.st_size + 511) / TAR_HEADER_SIZE * TAR_HEADER_SIZE);
        m_file->pwrite(&th_buf, TAR_HEADER_SIZE,
                       (s.st_size + 511) / TAR_HEADER_SIZE * TAR_HEADER_SIZE + TAR_HEADER_SIZE);
        return 0;
    }
}; // namespace FileSystem

class TarFs : public ForwardFS_Ownership {
public:
    TarFs(IFileSystem *fs) : ForwardFS_Ownership(fs, true) {
    }
    IFile *open(const char *pathname, int flags, mode_t mode) {
        IFile *file = m_fs->open(pathname, flags, mode);
        if (!file) {
            return nullptr;
        }
        if (flags & O_RDONLY) {
            return open_tar_file(file);
        }
        struct stat s;
        file->fstat(&s);
        if (s.st_size == 0) {
            mark_new_tar(file);
            return open_tar_file(file, false);
        }
        return open_tar_file(file);
    }

    IFile *open(const char *pathname, int flags) {
        IFile *file = m_fs->open(pathname, flags);
        if (!file) {
            return nullptr;
        }
        if (flags & O_RDONLY) {
            return open_tar_file(file);
        }
        struct stat s;
        file->fstat(&s);
        if (s.st_size == 0) {
            mark_new_tar(file);
            return open_tar_file(file, false);
        }
        return open_tar_file(file);
    }

private:
    bool mark_new_tar(IFile *file) {
        LOG_INFO("new tar header");
        struct tar_header th_buf;
        memset(&(th_buf), 0, sizeof(struct tar_header));
        snprintf(th_buf.name, 100, "%s", "overlaybd.new");
        strncpy(th_buf.version, TVERSION_EMPTY, TVERSLEN);
        strncpy(th_buf.magic, TMAGIC_EMPTY, TMAGLEN);
        int_to_oct_nonull(-1, th_buf.size, 12);
        return (file->pwrite((char *)(&th_buf), TAR_HEADER_SIZE, 0) == TAR_HEADER_SIZE);
    }
};


int is_tar_file(IFile *file) {

    struct tar_header th_buf;
    if (file->pread(&th_buf, TAR_HEADER_SIZE, 0) != TAR_HEADER_SIZE) {
        LOG_DEBUG("error read tar file header");
        return 0;
    }
    if (strncmp(th_buf.magic, TMAGIC, TMAGLEN - 1) != 0) {
        LOG_DEBUG("unknown magic value in tar header");
        return 0;
    }
    if (strncmp(th_buf.version, TVERSION, TVERSLEN) != 0) {
        LOG_DEBUG("unknown version value in tar header");
        return 0;
    }
    int crc = oct_to_int(th_buf.chksum);
    if (!(crc == th_crc_calc(th_buf) || crc == th_signed_crc_calc(th_buf))) {
        LOG_DEBUG("tar header checksum error");
        return 0;
    }
    return 1;
}

static IFile *new_tar_file(IFile *file) {

    auto ret = new TarFile(file);
    if (ret->read_header() != 0) {
        LOG_ERRNO_RETURN(0, nullptr, "read tar header failed.");
    }
    return ret;
}

static IFile *open_tar_file(IFile *file, bool verify_type) {

    if (!verify_type) {
        return new_tar_file(file);
    }
    if (is_tar_file(file) == 1) {
        return new_tar_file(file);
    }
    LOG_DEBUG("not tar file, open as normal file");
    return file; // open as normal file
}

IFileSystem *new_tar_fs_adaptor(IFileSystem *fs) {
    return new TarFs(fs);
}

IFile *new_tar_file_adaptor(IFile *file) {
    return open_tar_file(file);
}

