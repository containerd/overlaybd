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
#include <linux/falloc.h>
#include <algorithm>
#include <cstddef>
#include <new>
#include <set>
#include <string>
#include <vector>
#include <photon/fs/path.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/string_view.h>
#include <photon/common/estring.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/fiemap.h>
#include "../lsmt/file.h"
#include "../lsmt/index.h"
#include "fsmeta_file.h"

/*
 * Archive members may be stored with a leading / ("/usr/lib") or without
 * ("usr/lib"), while the extraction target is always mounted at its own root.
 * Root the name so that both flavours denote the very same path. This takes over
 * from the subfs("/") wrapper the target fs used to sit behind, which prepended
 * the slash unconditionally and so turned an already rooted "/usr/lib" into
 * "//usr/lib" -- an empty path element that ext2fs' namei() refuses to resolve.
 *
 * Names reach us cleaned by clean_name(), hence a rooted one carries neither an
 * empty element nor a ".." and can be taken as is. An unrooted one may still
 * begin with "..", which clean_name() only folds away for a rooted name; callers
 * reject those the way subfs' path_level_valid() did.
 */
std::string rooted_name(const char *name) {
    if (name[0] == '/') {
        return name;
    }
    return std::string("/") + name;
}

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

ssize_t UnTar::stage_tar(photon::fs::IFile *headers, photon::fs::IFile *target) {
    const off_t base = target->lseek(0, SEEK_END);
    if (base < 0) {
        LOG_ERRNO_RETURN(0, -1, "failed to locate the end of the upper data file");
    }
    const size_t BUF_SIZE = 1024 * 1024;
    void *mem = nullptr;
    if (posix_memalign(&mem, FS_BLOCKSIZE, BUF_SIZE) != 0) {
        LOG_ERROR_RETURN(ENOMEM, -1, "failed to allocate the staging buffer");
    }
    DEFER(free(mem));
    auto buf = (char *)mem;
    size_t buffered = 0;
    off_t staged = 0;
    auto flush = [&]() -> int {
        if (buffered == 0) {
            return 0;
        }
        if (target->pwrite(buf, buffered, base + staged) != (ssize_t)buffered) {
            LOG_ERRNO_RETURN(EIO, -1, "failed to stage ` bytes", buffered);
        }
        staged += buffered;
        buffered = 0;
        return 0;
    };

    ssize_t count = 0;
    int ret;
    while ((ret = read_header(headers)) == 0) {
        count++;
        size_t size = get_size();
        size_t left = (size + T_BLOCKSIZE - 1) & T_BLOCKMASK;
        if (!TH_ISREG(header) || left == 0) {
            continue;
        }
        auto end = headers->lseek(0, SEEK_CUR);
        if (end < T_BLOCKSIZE) {
            LOG_ERRNO_RETURN(0, -1, "failed to locate the dumped header of `", get_pathname());
        }
        size_t pad = (size_t)(-(base + staged + buffered)) & (FS_BLOCKSIZE - 1);
        memset(buf + buffered, 0, pad);
        buffered += pad;
        if (buffered == BUF_SIZE && flush() != 0) {
            return -1;
        }
        off_t roffset = base + staged + buffered;
        if (headers->pwrite(&roffset, sizeof(roffset),
                            end - T_BLOCKSIZE + offsetof(TarHeader, devmajor)) !=
            (ssize_t)sizeof(roffset)) {
            LOG_ERRNO_RETURN(0, -1, "failed to record the offset of staged data");
        }
        while (left > 0) {
            auto n = std::min(left, BUF_SIZE - buffered);
            if (file->read(buf + buffered, n) != (ssize_t)n) {
                LOG_ERRNO_RETURN(EIO, -1, "failed to read the data of `", get_pathname());
            }
            if (left == n) {
                // Tar padding is arbitrary, but mapped block tails must be zero.
                size_t tail = (size_t)(-size) & (T_BLOCKSIZE - 1);
                memset(buf + buffered + n - tail, 0, tail);
            }
            buffered += n;
            left -= n;
            if (buffered == BUF_SIZE && flush() != 0) {
                return -1;
            }
        }
    }
    if (ret != 1) {
        LOG_ERRNO_RETURN(0, -1, "failed to read tar header");
    }
    if (flush() != 0) {
        return -1;
    }
    if (staged > 0) {
        // the fs metadata appended next begins at a block boundary
        size_t pad = (size_t)(-(base + staged)) & (FS_BLOCKSIZE - 1);
        if (pad > 0) {
            memset(buf, 0, pad);
            if (target->pwrite(buf, pad, base + staged) != (ssize_t)pad) {
                LOG_ERRNO_RETURN(EIO, -1, "failed to pad the staged data");
            }
        }
    }
    LOG_INFO("staged ` bytes of file data at offset ` of the top layer, ` objects", staged, base,
             count);
    return count;
}

int UnTar::extract_staged(FsMetaFile *fsmeta, const std::function<void()> &close_fs) {
    if (fsmeta != nullptr && !close_fs) {
        LOG_ERROR_RETURN(EINVAL, -1, "fs metadata staging requires closing ext4 before commit");
    }
    auto lsmt = dynamic_cast<LSMT::IFileRW *>(fs_base_file);
    if (lsmt == nullptr || lsmt->can_map_staged_data() != 0) {
        LOG_ERROR_RETURN(ENOTSUP, -1, "the base file cannot map staged data");
    }
    // stage_tar() rewrote devmajor of the regular file headers
    options |= TAR_IGNORE_CRC;
    meta_only = from_tar_idx = defer_mapping = true;
    pending.clear();
    if (extract_all() != 0) {
        return -1;
    }
    // a freed metadata block reused for file data may still be cached dirty until now
    if (fs->syncfs() != 0) {
        LOG_ERRNO_RETURN(0, -1, "failed to sync fs metadata");
    }
    if (close_fs) {
        close_fs();
        fs = nullptr;
        xattr_fs = nullptr;
    }
    if (fsmeta != nullptr) {
        // Ext4 allocates whole blocks, so a payload extent cannot share a block with metadata.
        for (auto &it : pending) {
            for (auto &lba : it.second) {
                fsmeta->discard(lba.offset, lba.count);
            }
        }
        if (fsmeta->commit() != 0) {
            return -1;
        }
    }
    return apply_pending();
}

int UnTar::apply_pending() {
    auto lsmt = static_cast<LSMT::IFileRW *>(fs_base_file);
    size_t mapped = 0, zeroed = 0;
    for (auto &it : pending) {
        for (auto &lba : it.second) {
            if (lba.roffset >= 0) {
                if (fs_base_file->ioctl(LSMT::IFileRW::RemoteData, lba) != (int)lba.count) {
                    LOG_ERRNO_RETURN(0, -1, "failed to map staged data, offset: `, count: `, roffset: `",
                                     lba.offset, lba.count, lba.roffset);
                }
                mapped += lba.count;
                continue;
            }
            // the tail of a last block reads as zero, unless the block is a reused one
            LSMT::Segment s{(uint64_t)lba.offset / T_BLOCKSIZE, lba.count / T_BLOCKSIZE};
            std::vector<LSMT::SegmentMapping> pm(s.length);
            auto n = lsmt->index()->lookup(s, pm.data(), pm.size());
            if (std::any_of(pm.begin(), pm.begin() + n, [](const LSMT::SegmentMapping &m) {
                    return !m.zeroed;
                })) {
                if (fs_base_file->fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, lba.offset,
                                            lba.count) != 0) {
                    LOG_ERRNO_RETURN(0, -1, "failed to zero stale data, offset: `, count: `",
                                     lba.offset, lba.count);
                }
                zeroed += lba.count;
            }
        }
    }
    pending.clear();
    LOG_INFO("mapped ` bytes of staged data, zeroed ` bytes of stale block tails", mapped, zeroed);
    return 0;
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
        if (!photon::fs::path_level_valid(name)) {
            LOG_ERROR_RETURN(EINVAL, -1, "filename ` climbs above the extraction root", name);
        }
        std::string filename = rooted_name(name);
        if (filename == "/") {
            LOG_WARN("file '`' ignored: resolved to root", name);
            continue;
        }
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
                if (unlink_file(filename, s) == -1 && errno != ENOENT) {
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

int UnTar::unlink_file(const char *path, const struct stat &st) {
    if (fs->unlink(path) != 0) {
        return -1;
    }
    // the last link frees the blocks of the inode for reuse
    if (S_ISREG(st.st_mode) && st.st_nlink <= 1) {
        pending.erase(st.st_ino);
    }
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
        memcpy(&p, header.devmajor, sizeof(p));
    } else {
        p = file->lseek(0, SEEK_CUR);
    }
    // an empty file owns no block
    if (size > 0 && map_regfile_data(fout, size, p) != 0) {
        LOG_ERRNO_RETURN(0, -1, "failed to map the data of `", filename);
    }
    if (not from_tar_idx) {
        file->lseek(((size+ T_BLOCKSIZE - 1) / T_BLOCKSIZE) * T_BLOCKSIZE, SEEK_CUR); // skip size
    }
    return 0;
}

// maps the blocks allocated for the file to its data at offset of the base file's backing data
int UnTar::map_regfile_data(photon::fs::IFile *fout, size_t size, off_t offset) {
    if (fout->fallocate(0, 0, size) != 0) {
        return -1;
    }
    photon::fs::fiemap probe(0, size, 0);
    if (fout->fiemap(&probe) != 0) {
        return -1;
    }
    std::vector<char> mem(sizeof(photon::fs::fiemap) +
                          probe.fm_mapped_extents * sizeof(photon::fs::fiemap_extent));
    auto fie = new (mem.data()) photon::fs::fiemap(0, size, probe.fm_mapped_extents);
    if (fout->fiemap(fie) != 0) {
        return -1;
    }

    std::vector<LSMT::RemoteMapping> *deferred = nullptr;
    if (defer_mapping) {
        struct stat st;
        if (fout->fstat(&st) != 0) {
            return -1;
        }
        deferred = &pending[st.st_ino];
        deferred->clear();
    }
    size_t alignment = defer_mapping ? FS_BLOCKSIZE : T_BLOCKSIZE;
    size_t data_len = (size + alignment - 1) & ~(alignment - 1);
    // extents come in file order, and a file allocated in full has no hole
    size_t pos = 0;
    for (uint32_t i = 0; i < fie->fm_mapped_extents; i++) {
        auto &e = fie->fm_extents[i];
        size_t len = pos < data_len ? std::min((size_t)e.fe_length, data_len - pos) : 0;
        if (len > 0) {
            LSMT::RemoteMapping lba{(off_t)e.fe_physical, (uint32_t)len, offset + (off_t)pos};
            if (deferred) {
                deferred->push_back(lba);
            } else if (fs_base_file->ioctl(LSMT::IFileRW::RemoteData, lba) != (int)len) {
                return -1;
            }
        }
        if (deferred && len < e.fe_length) {
            deferred->push_back({(off_t)(e.fe_physical + len), (uint32_t)(e.fe_length - len), -1});
        }
        pos += e.fe_length;
    }
    if (pos < data_len) {
        LOG_ERROR_RETURN(EIO, -1, "only ` bytes are allocated for ` bytes", pos, size);
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
    // a hardlink target is a path inside the target fs, unlike a symlink target
    char *linkname = get_linkname();
    if (!photon::fs::path_level_valid(linkname)) {
        LOG_ERROR_RETURN(EINVAL, -1, "link target ` climbs above the extraction root", linkname);
    }
    auto linktgt = rooted_name(linkname);
    LOG_DEBUG("  ==> extracting: ` (link to `)", filename, linktgt);
    if (fs->link(linktgt.c_str(), filename) == -1) {
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
