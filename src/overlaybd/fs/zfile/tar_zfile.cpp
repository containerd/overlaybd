/*
  * tar_zfile.cpp
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
#include "tar_zfile.h"

#include "../../alog.h"
#include "../../fs/filesystem.h"
#include "../../fs/forwardfs.h"
#include "../../fs/zfile/zfile.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>

namespace FileSystem {

/* Contents of magic field and its length.  */
#define TMAGIC "ustar"
#define TMAGLEN 6

/* Contents of the version field and its length.  */
#define TVERSION "00"
#define TVERSLEN 2

#define TMAGIC_EMPTY "xxtar"
#define TVERSION_EMPTY "xx"

#define TAR_HEADER_SIZE 512

#define int_to_oct(num, oct, octlen) \
    snprintf((oct), (octlen), "%*lo ", (octlen)-2, (unsigned long)(num))

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

int oct_to_int(char *oct) {
    int i;
    return sscanf(oct, "%o", &i) == 1 ? i : 0;
}
void int_to_oct_nonull(int num, char *oct, size_t octlen) {
    snprintf(oct, octlen, "%*lo", (int)(octlen - 1), (unsigned long)num);
    oct[octlen - 1] = ' ';
}
size_t oct_to_size(char *oct) {
    size_t i;
    return sscanf(oct, "%zo", &i) == 1 ? i : 0;
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
        base_offset = TAR_HEADER_SIZE;
        m_file->lseek(base_offset, SEEK_SET);
    }

    ~TarFile() {
        close();
    }

    virtual int fstat(struct stat *buf) override {
        struct tar_header th_buf;
        memset(&(th_buf), 0, sizeof(struct tar_header));
        m_file->pread(&th_buf, TAR_HEADER_SIZE, 0);
        size_t size = oct_to_size(th_buf.size);

        int ret = m_file->fstat(buf);
        if (ret < 0)
            return ret;
        if (size == -1) // new tar
            buf->st_size -= base_offset;
        else
            buf->st_size = size;
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
        offset += base_offset;
        return m_file->pread(buf, count, offset);
    }

    virtual int close() override {
        struct stat s;
        m_file->fstat(&s);
        struct tar_header th_buf;
        memset(&(th_buf), 0, sizeof(struct tar_header));
        m_file->pread(&th_buf, TAR_HEADER_SIZE, 0);
        if (strncmp(th_buf.magic, TMAGIC_EMPTY, TMAGLEN - 1) == 0 && strncmp(th_buf.version, TVERSION_EMPTY, TVERSLEN) == 0) {
            LOG_INFO("write header for tar file");
            write_header_trailer();
        }
        return m_file->close();
    }

private:
    off_t base_offset;
    int write_header_trailer() {
        struct stat s;
        m_file->fstat(&s);
        struct tar_header th_buf;
        memset(&(th_buf), 0, sizeof(struct tar_header));
        //th_set_type
        th_buf.typeflag = '0';
        //th_set_user
        struct passwd *pw;
        pw = getpwuid(s.st_uid);
        if (pw != NULL)
            strlcpy(th_buf.uname, pw->pw_name, sizeof(th_buf.uname));
        int_to_oct(s.st_uid, th_buf.uid, 8);
        //th_set_group
        struct group *gr;
        gr = getgrgid(s.st_gid);
        if (gr != NULL)
            strlcpy(th_buf.gname, gr->gr_name, sizeof(th_buf.gname));
        int_to_oct(s.st_gid, th_buf.gid, 8);
        //th_set_mode
        int_to_oct(s.st_mode, th_buf.mode, 8);
        //th_set_mtime
        int_to_oct_nonull(s.st_mtime, th_buf.mtime, 12);
        //th_set_size
        int_to_oct_nonull(s.st_size - TAR_HEADER_SIZE, th_buf.size, 12);
        //th_set_path
        snprintf(th_buf.name, 100, "%s", "overlaybd.commit");

        /* magic, version, and checksum */
        strncpy(th_buf.version, TVERSION, TVERSLEN);
        strncpy(th_buf.magic, TMAGIC, TMAGLEN);
        int_to_oct(th_crc_calc(th_buf), th_buf.chksum, 8);

        m_file->pwrite(&th_buf, TAR_HEADER_SIZE, 0);

        memset(&(th_buf), 0, sizeof(struct tar_header));
        m_file->pwrite(&th_buf, TAR_HEADER_SIZE, (s.st_size + 511) / TAR_HEADER_SIZE * TAR_HEADER_SIZE);
        m_file->pwrite(&th_buf, TAR_HEADER_SIZE, (s.st_size + 511) / TAR_HEADER_SIZE * TAR_HEADER_SIZE + TAR_HEADER_SIZE);
        return 0;
    }
}; // namespace FileSystem

class TarZfileFs : public ForwardFS_Ownership {
public:
    TarZfileFs(IFileSystem *fs) : ForwardFS_Ownership(fs, true) {
    }
    IFile *open(const char *pathname, int flags, mode_t mode) {
        IFile *file = m_fs->open(pathname, flags, mode);
        if (!file) {
            return nullptr;
        }
        if (flags & O_RDONLY) {
            return open_tar_zfile(file, pathname);
        }
        struct stat s;
        file->fstat(&s);
        if (s.st_size == 0) {
            mark_new_tar(file);
            return new TarFile(file);
        }
        return open_tar_zfile(file, pathname);
    }

    IFile *open(const char *pathname, int flags) {
        IFile *file = m_fs->open(pathname, flags);
        if (!file) {
            return nullptr;
        }
        if (flags & O_RDONLY) {
            return open_tar_zfile(file, pathname);
        }
        struct stat s;
        file->fstat(&s);
        if (s.st_size == 0) {
            mark_new_tar(file);
            return new TarFile(file);
        }
        return open_tar_zfile(file, pathname);
    }

private:
    bool mark_new_tar(IFile *file) {
        struct tar_header th_buf;
        memset(&(th_buf), 0, sizeof(struct tar_header));
        snprintf(th_buf.name, 100, "%s", "overlaybd.new");
        strncpy(th_buf.version, TVERSION_EMPTY, TVERSLEN);
        strncpy(th_buf.magic, TMAGIC_EMPTY, TMAGLEN);
        int_to_oct_nonull(-1, th_buf.size, 12);
        return (file->pwrite((char *)(&th_buf), TAR_HEADER_SIZE, 0) == TAR_HEADER_SIZE);
    }

    TarFile *open_tar_file(IFile *file) {
        if (is_tar_file(file) == 1) {
            return new TarFile(file);
        }
        return nullptr;
    }

    IFile *open_tar_zfile(IFile *file, const char *path) {
        if (ZFile::is_zfile(file) == 1) {
            //a zfile without tar
            auto zf = ZFile::zfile_open_ro(file, true, true);
            if (!zf) {
                delete file;
                LOG_ERROR_RETURN(0, nullptr, "zfile_open_ro(`) failed, `:`", path, errno, strerror(errno));
            }
            return zf;
        } else {
            TarFile *tfile = open_tar_file(file);
            if (!tfile) {
                delete file;
                LOG_ERROR_RETURN(0, nullptr, "open_tar_file(`) failed, `:`", path, errno, strerror(errno));
            }
            if (ZFile::is_zfile(tfile) == 1) {
                // a zfile in tar
                auto zf = ZFile::zfile_open_ro(tfile, true, true);
                if (!zf) {
                    delete tfile;
                    LOG_ERROR_RETURN(0, nullptr, "zfile_open_ro(`) failed, `:`", path, errno, strerror(errno));
                }
                return zf;
            } else {
                // not a zfile in tar
                delete tfile;
                LOG_ERROR_RETURN(0, nullptr, "not a zfile in tar: `", path);
            }
        }
        return nullptr;
    }
};

IFileSystem *new_tar_zfile_fs_adaptor(IFileSystem *fs) {
    return new TarZfileFs(fs);
}

int is_tar_file(IFile *file) {
    struct tar_header th_buf;
    if (file->pread(&th_buf, TAR_HEADER_SIZE, 0) != TAR_HEADER_SIZE) {
        LOG_ERROR_RETURN(0, -1, "error read tar file header");
    }
    if (strncmp(th_buf.magic, TMAGIC, TMAGLEN - 1) != 0) {
        LOG_ERROR_RETURN(0, 0, "unknown magic value in tar header");
    }
    if (strncmp(th_buf.version, TVERSION, TVERSLEN) != 0) {
        LOG_ERROR_RETURN(0, 0, "unknown version value in tar header");
    }
    int crc = oct_to_int(th_buf.chksum);
    if (!(crc == th_crc_calc(th_buf) || crc == th_signed_crc_calc(th_buf))) {
        LOG_ERROR_RETURN(0, 0, "tar header checksum error");
    }
    return 1;
}

int is_tar_zfile(IFile *file) {
    auto tfile = new_tar_file_adaptor(file);
    if (!tfile) return -1;
    return ZFile::is_zfile(tfile);
}

IFile *new_tar_file_adaptor(IFile *file) {
    if (is_tar_file(file) == 1) {
        return new TarFile(file);
    }
    return nullptr;
}

} // namespace FileSystem