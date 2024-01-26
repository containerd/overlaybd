/*
**  Copyright 2022 overlaybd authors
**  Copyright 1998-2003 University of Illinois Board of Trustees
**  Copyright 1998-2003 Mark D. Roth
**  All rights reserved.
**
**  libtar.c - demo driver program for libtar
**
**  Mark D. Roth <roth@uiuc.edu>
**  Campus Information Technologies and Educational Services
**  University of Illinois at Urbana-Champaign
*/

#include "libtar.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <utime.h>
#include <set>
#include <string>
#include <photon/fs/path.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/string_view.h>
#include <photon/common/estring.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/fiemap.h>
#include "../lsmt/file.h"
#include "../lsmt/index.h"

int UnTar::set_file_perms(const char *filename) {
    mode_t mode = header.get_mode();
    uid_t uid = header.get_uid();
    gid_t gid = header.get_gid();
    struct timeval tv[2];
    tv[0].tv_sec = tv[1].tv_sec = header.get_mtime();
    tv[0].tv_usec = tv[1].tv_usec = 0;

    /* change owner/group */
    if (!BIT_ISSET(options, TAR_CHECK_EUID) || geteuid() == 0) {
        if (fs->lchown(filename, uid, gid) == -1) {
            LOG_ERRNO_RETURN(0, -1, "lchown failed, filename `, uid `, gid `", filename, uid, gid);
        }
    }

    /* change xattrs */
    if (xattr_fs && pax && !pax->records.empty()) {
        std::string prefix(PAX_SCHILY_XATTR_PREFIX);
        for (auto &rec : pax->records) {
            if (estring_view(rec.first).starts_with(prefix)) {
                auto name = rec.first.substr(prefix.size());
                auto value = rec.second;
                LOG_DEBUG(VALUE(name), VALUE(value), VALUE(value.size()));
                if (xattr_fs->lsetxattr(filename, name.c_str(), value.c_str(), value.size(), 0) == -1) {
                    ERRNO eno;
                    if (eno.no == EPERM && estring_view(name).starts_with("user.")) {
                        // In the user.* namespace, only regular files and directories can have extended attributes.
                        // See https://man7.org/linux/man-pages/man7/xattr.7.html for details.
                        if (!TH_ISREG(header) && !TH_ISDIR(header)) {
                            LOG_WARN("ignored xattr '`' in archive ", name, VALUE(eno));
                            continue;
                        }
                    } else if (eno.no == ENOTSUP) {
                        LOG_WARN("ignored xattr '`' in archive ", name, VALUE(eno));
                        continue;
                    }
                    LOG_ERRNO_RETURN(eno.no, -1, "lsetxattr failed, filename `, name `, value `", filename, name, value);
                }
            }
        }
    }

    /* change access/modification time */
    if (fs->lutimes(filename, tv) == -1) {
        LOG_ERRNO_RETURN(0, -1, "lutimes failed, filename `", filename);
    }

    /* change permissions */
    // skip symlink
    // NOTE: Allow hardlink to the softlink, not the real one. For example,
    //
    //	touch /tmp/zzz
    //	ln -s /tmp/zzz /tmp/xxx
    //	ln /tmp/xxx /tmp/yyy
    //
    // /tmp/yyy should be softlink which be same of /tmp/xxx, not /tmp/zzz.
    struct stat s;
    if (fs->lstat(filename, &s) == 0 && S_ISLNK(s.st_mode)) {
        return 0;
    }
    if (fs->chmod(filename, mode) == -1) {
        LOG_ERRNO_RETURN(0, -1, "chmod failed, filename `, mode `", filename, mode);
    }

    return 0;
}

ssize_t UnTar::dump_tar_headers(photon::fs::IFile *as) {
    ssize_t count = 0;
    while (true) {
        auto next = read_header(as);
        if (next == -1) {
            return -1;
        } else if (next == 1) {
            break;
        }
        count++;
        if (TH_ISREG(header)) {
            auto size = get_size();
            file->lseek(((size + T_BLOCKSIZE - 1) / T_BLOCKSIZE) * T_BLOCKSIZE, SEEK_CUR); // skip size
        }
    }
    return count;
}

int UnTar::extract_all() {
    int i, count = 0;
    unpackedPaths.clear();
    dirs.clear();

    while ((i = read_header()) == 0) {
        auto name = get_pathname(); // cleaned name
        if (name == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "get filename failed");
        }
        if (strcmp(name, "/") == 0) {
            LOG_WARN("file '/' ignored: resolved to root");
            continue;
        }
        std::string filename(name);
        if (extract_file(filename.c_str()) != 0) {
            LOG_ERRNO_RETURN(0, -1, "extract failed, filename `", filename);
        }
        count++;
    }

    // change time for all dir
    for (auto dir : dirs) {
        std::string path = dir.first;
        struct timeval tv[2];
        tv[0].tv_sec = tv[1].tv_sec = dir.second;
        tv[0].tv_usec = tv[1].tv_usec = 0;
        if (fs->lutimes(path.c_str(), tv) == -1) {
            LOG_ERRNO_RETURN(0, -1, "utime failed, filename `", dir.first.c_str());
        }
    }

    LOG_INFO("extract ` file", count);

    return (i == 1 ? 0 : -1);
}

int UnTar::extract_file(const char *filename) {
    int i;

    // ensure parent directory exists or is created.
    photon::fs::Path p(filename);
    if (mkdir_hier(p.dirname()) < 0) {
        return -1;
    }

    // whiteout files by removing the target files.
    auto cwres = convert_whiteout(filename);
    if (cwres < 0) {
        return -1;
    }
    if (cwres == 1) {
        return 0;
    }

    // check file exist
    struct stat s;
    if (fs->lstat(filename, &s) == 0 || errno != ENOENT) {
        if (BIT_ISSET(options, TAR_NOOVERWRITE)) {
            errno = EEXIST;
            return -1;
        } else {
            if (!S_ISDIR(s.st_mode)) {
                if (fs->unlink(filename) == -1 && errno != ENOENT) {
                    LOG_ERRNO_RETURN(EEXIST, -1, "remove exist file ` failed", filename);
                }
            } else if (!TH_ISDIR(header)) {
                if (remove_all(filename) == -1) {
                    LOG_ERRNO_RETURN(EEXIST, -1, "remove exist dir ` failed", filename);
                }
            }
        }
    }

    if (TH_ISDIR(header)) {
        i = extract_dir(filename);
        if (i == 1)
            i = 0;
    }
    else if (TH_ISREG(header))
        i = extract_regfile(filename);
    else if (TH_ISLNK(header))
        i = extract_hardlink(filename);
    else if (TH_ISSYM(header))
        i = extract_symlink(filename);
    else if (TH_ISCHR(header) || TH_ISBLK(header)) {
        if (!BIT_ISSET(options, TAR_CHECK_EUID) || geteuid() == 0) {
            i = extract_block_char_fifo(filename);
        } else {
            LOG_WARN("file ` ignored: skip for user namespace", filename);
            return 0;
        }
    }
    else if (TH_ISFIFO(header))
        i = extract_block_char_fifo(filename);
    else {
        LOG_ERROR("unhandled tar header type `", header.typeflag);
        return 1;
    }

    if (i != 0) {
        return i;
    }

    i = set_file_perms(filename);
    if (i != 0) {
        return i;
    }
    // Directory mtimes must be handled at the end to avoid further
    // file creation in them to modify the directory mtime
    if (TH_ISDIR(header)) {
        dirs.emplace_back(std::make_pair(filename, header.get_mtime()));
    }
    unpackedPaths.insert(filename);
    return 0;
}

int UnTar::extract_regfile_meta_only(const char *filename) {
    size_t size = get_size();
    LOG_DEBUG("  ==> extracting: ` (` bytes) (turboOCIv1 index)", filename, size);
    photon::fs::IFile *fout = fs->open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0666);
    if (fout == nullptr) {
        return -1;
    }
    DEFER({delete fout;});

    off_t p = 0;
    if (from_tar_idx) {
        p = *((off_t*)&header.devmajor);
    } else {
        p = file->lseek(0, SEEK_CUR);
    }
    struct photon::fs::fiemap_t<8192> fie(0, size);

    if (fout->fallocate(0, 0, size) != 0 || fout->fiemap(&fie)!=0 ){
        return -1;
    }
    auto count = ((size+ T_BLOCKSIZE - 1) / T_BLOCKSIZE) * T_BLOCKSIZE;
    for (uint32_t i = 0; i < fie.fm_mapped_extents; i++) {
        LSMT::RemoteMapping lba;
        lba.offset = fie.fm_extents[i].fe_physical;
        lba.count = (fie.fm_extents[i].fe_length < count ? fie.fm_extents[i].fe_length : count);
        lba.roffset = p;
        int nwrite = fs_base_file->ioctl(LSMT::IFileRW::RemoteData, lba);
        if (nwrite < 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to write lba");
        }
        p += nwrite;
        count-=lba.count;
    }

    struct stat st;
    fout->fstat(&st);
    LOG_DEBUG("reg file size `", st.st_size);
    if (not from_tar_idx) {
        file->lseek(((size+ T_BLOCKSIZE - 1) / T_BLOCKSIZE) * T_BLOCKSIZE, SEEK_CUR); // skip size
    }
    return 0;
}

int UnTar::extract_regfile(const char *filename) {
    if (meta_only) {
        return extract_regfile_meta_only(filename);
    }

    size_t size = get_size();

    LOG_DEBUG("  ==> extracting: ` (` bytes)", filename, size);
    photon::fs::IFile *fout = fs->open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0666);
    if (fout == nullptr) {
        return -1;
    }
    DEFER({ delete fout; });

    char buf[1024 * 1024];
    off_t pos = 0;
    size_t left = size;
    while (left > 0) {
        size_t rsz;
        if (left > 1024 * 1024)
            rsz = 1024 * 1024;
        else if (left > fs_blocksize)
            rsz = left & fs_blockmask;
        else
            rsz = (left & ~T_BLOCKMASK) ? (left & T_BLOCKMASK) + T_BLOCKSIZE : (left & T_BLOCKMASK);
        if (file->read(buf, rsz) != (ssize_t)rsz) {
            LOG_ERRNO_RETURN(0, -1, "failed to read block");
        }
        size_t wsz = (left < rsz) ? left : rsz;
        if (fout->pwrite(buf, wsz, pos) != (ssize_t)wsz) {
            LOG_ERRNO_RETURN(0, -1, "failed to write file");
        }
        pos += wsz;
        left -= wsz;
        // LOG_DEBUG(VALUE(rsz), VALUE(wsz), VALUE(pos), VALUE(left));
    }
    return 0;
}

int UnTar::extract_hardlink(const char *filename) {
    char *linktgt = get_linkname();
    LOG_DEBUG("  ==> extracting: ` (link to `)", filename, linktgt);
    if (fs->link(linktgt, filename) == -1) {
        LOG_ERRNO_RETURN(0, -1, "link failed, filename `, linktgt `", filename, linktgt);
    }
    return 0;
}

int UnTar::extract_symlink(const char *filename) {
    char *linktgt = get_linkname();
    LOG_DEBUG("  ==> extracting: ` (symlink to `)", filename, linktgt);
    if (fs->symlink(linktgt, filename) == -1) {
        LOG_ERRNO_RETURN(0, -1, "symlink failed, filename `, linktgt `", filename, linktgt);
    }
    return 0;
}

int UnTar::extract_dir(const char *filename) {
    mode_t mode = header.get_mode();

    LOG_DEBUG("  ==> extracting: ` (mode `, directory)", filename, OCT(mode));
    if (fs->mkdir(filename, mode) < 0) {
        if (errno == EEXIST) {
            return 1;
        } else {
            return -1;
        }
    }
    return 0;
}

int UnTar::extract_block_char_fifo(const char *filename) {
    auto mode = header.get_mode();
    auto devmaj = header.get_devmajor();
    auto devmin = header.get_devminor();

    LOG_DEBUG("  ==> extracting: ` (block/char/fifo `,`)", filename, devmaj, devmin);
    if (fs->mknod(filename, mode, makedev(devmaj, devmin)) == -1) {
        LOG_ERRNO_RETURN(0, -1, "block/char/fifo failed, filename `", filename);
    }

    return 0;
}
