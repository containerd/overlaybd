#include "liberofs.h"
#include "erofs/tar.h"
#include "erofs/io.h"
#include "erofs/cache.h"
#include "erofs/blobchunk.h"
#include "erofs/block_list.h"
#include "erofs/inode.h"
#include "erofs/config.h"
#include "erofs/dir.h"
#include "../../lsmt/file.h"
#include "../../lsmt/index.h"
#include <photon/common/alog.h>
#include <map>
#include <set>
#include <dirent.h>

#define SECTOR_SIZE 512ULL
#define SECTOR_BITS 9
#define round_down_blk(addr) ((addr) & (~(SECTOR_SIZE - 1)))
#define round_up_blk(addr) (round_down_blk((addr) + SECTOR_SIZE - 1))
#define min(a, b) (a) < (b) ? (a) : (b)
#define EROFS_ROOT_XATTR_SZ (16 * 1024)

#define EROFS_UNIMPLEMENTED 1

struct liberofs_inmem_sector {
    char data[SECTOR_SIZE];
};

class ErofsCache {
public:
    ErofsCache(photon::fs::IFile *file, unsigned long int capacity):
    file(file), capacity(capacity)
    {}
    ~ErofsCache() {}
    ssize_t write_sector(u64 addr, char *buf);
    ssize_t read_sector(u64 addr, char *buf);
    int flush();
public:
    photon::fs::IFile *file;
    long unsigned int capacity;
    std::map<u64, struct liberofs_inmem_sector*>caches;
    std::set<u64> dirty;
};

struct liberofs_file {
    struct erofs_vfops ops;
    photon::fs::IFile *file;
    ErofsCache *cache;
};

ssize_t ErofsCache::write_sector(u64 addr, char *buf)
{
    struct liberofs_inmem_sector *sector;

    if (addr & (SECTOR_SIZE-1)) {
        LOG_ERROR("Invalid addr, should be aligned to SECTOR_SIZE.");
        return -1;
    }
    if (caches.find(addr) != caches.end()) {
        sector = caches[addr];
        memcpy(sector->data, buf, SECTOR_SIZE);
        dirty.insert(addr);
    } else {
        if (caches.size() == capacity) {
            auto it = caches.begin();
            if (dirty.find(it->first) != dirty.end()) {
                if (file->pwrite(it->second->data, SECTOR_SIZE, it->first)
                    != SECTOR_SIZE)
                {
                    LOG_ERROR("Fail to write sector %lld.", it->first);
                    return -EIO;
                }
                dirty.erase(it->first);
            }
            sector = it->second;
            caches.erase(it);
        } else  {
            sector = (struct liberofs_inmem_sector*)malloc(
                            sizeof(struct liberofs_inmem_sector));
            if (!sector)
                return -ENOMEM;
        }
        memcpy(sector->data, buf, SECTOR_SIZE);
        caches[addr] = sector;
        dirty.insert(addr);
    }
    return SECTOR_SIZE;
}

ssize_t ErofsCache::read_sector(u64 addr, char *buf)
{
    struct liberofs_inmem_sector *sector;

    if (addr & (SECTOR_SIZE - 1)) {
        LOG_ERROR("Invalid addr, should be aligned to SECTOR_SIZE.");
        return -1;
    }

    if (caches.find(addr) != caches.end()) {
        sector = caches[addr];
        memcpy(buf, sector->data, SECTOR_SIZE);
    } else {
        if (caches.size() == capacity) {
            auto it = caches.begin();
            if (dirty.find(it->first) != dirty.end()) {
                sector = it->second;
                if (file->pwrite(sector->data, SECTOR_SIZE, it->first)
                    != SECTOR_SIZE)
                {
                    LOG_ERROR("Fail to write sector %lld.", it->first);
                    return -EIO;
                }
                dirty.erase(it->first);
            }
            sector = it->second;
            caches.erase(it);
        } else {
            sector = (struct liberofs_inmem_sector*)malloc(
                            sizeof(struct liberofs_inmem_sector));
            if (!sector)
                return -ENOMEM;
        }
        if (file->pread(sector->data, SECTOR_SIZE, addr) != SECTOR_SIZE) {
            LOG_ERROR("Fail to read sector %lld", addr);
            return -EIO;
        }
        caches[addr] = sector;
        memcpy(buf, sector->data, SECTOR_SIZE);
    }
    return SECTOR_SIZE;
}

int ErofsCache::flush()
{
    for (auto it = caches.begin(); it != caches.end();) {
        if (dirty.find(it->first) != dirty.end()) {
            if (file->pwrite(it->second->data, SECTOR_SIZE, it->first)
                != SECTOR_SIZE)
            {
                LOG_ERROR("Fail to flush sector %lld.\n", it->first);
                return -1;
            }
            dirty.erase(it->first);
        }
        free(it->second);
        it = caches.erase(it);
    }
    return dirty.size() != 0 || caches.size() != 0;
}

/*
 * Helper function for reading from the photon file, since
 * the photon file requires reads to be 512-byte aligned.
 */
static ssize_t erofs_read_photon_file(void *buf, u64 offset, size_t len,
                                      ErofsCache *cache)
{

    ssize_t read;
    u64 start, end;
    char extra_buf[SECTOR_SIZE];
    u64 i, j;

    start = round_down_blk(offset);
    end = round_up_blk(offset + len);
    read = 0;

    /* we use the extra_buf to store the first sector or last sector when:
     * - start != offset
     * or
     * - end != offset + len
     */
    if (start != offset || end != offset + len) {
        i = start == offset ? start : start + SECTOR_SIZE;
        j = end == offset + len ? end : end - SECTOR_SIZE;

        /* read the first sector */
        if (i != start) {
            if (cache->read_sector(start, extra_buf) != SECTOR_SIZE) {
                LOG_ERROR("Fail to read start sector.");
                return -1;
            }
            memcpy((char*)buf, extra_buf + offset - start,
                        min(start + SECTOR_SIZE - offset,len));
            read += min(start + SECTOR_SIZE - offset,len);
        }

        /* read the last sector and avoid re-reading the same sector as above */
        if (j != end && (i == start || end - start > SECTOR_SIZE)) {
            if (cache->read_sector(end - SECTOR_SIZE,
                extra_buf) != SECTOR_SIZE)
            {
                LOG_ERROR("Fail to read start sector.");
                return -1;
            }
            memcpy((char*)buf + end - SECTOR_SIZE - offset, extra_buf,
                                        len + offset + SECTOR_SIZE - end);
            read += len + offset + SECTOR_SIZE - end;
        }

        for (u64 addr = i; addr < j; addr += SECTOR_SIZE) {
            if (cache->read_sector(addr,
                                   addr - offset + (char*)buf) != SECTOR_SIZE)
            {
                LOG_ERROR("Fail to read sector %lld in read_photo_file.\n", i);
                return -1;
            }
            read += SECTOR_SIZE;
        }
    } else {
        /* if read request is sector-aligned, we use the original buffer */
        for (u64 i = start; i < end; i += SECTOR_SIZE) {
            if (cache->read_sector(i, (char*)buf + i - start) != SECTOR_SIZE) {
                LOG_ERROR("Fail to read start sector.");
                return -1;
            }
            read += SECTOR_SIZE;
        }
    }

	return read;
}

/*
 * Helper function for writing to a photon file.
 */
 static ssize_t erofs_write_photon_file(const void *buf, u64 offset,
                                        size_t len, ErofsCache *cache)
 {
    ssize_t write;
    u64 start, end;
    char extra_buf[SECTOR_SIZE];
    u64 i, j;

    start = round_down_blk(offset);
    end = round_up_blk(offset + len);
    write = 0;

    if (start != offset || end != offset + len) {
        i = start == offset ? start : start + SECTOR_SIZE;
        j = end == offset + len ? end : end - SECTOR_SIZE;

        if (i != start) {
            if (cache->read_sector(start, extra_buf) != SECTOR_SIZE) {
                LOG_ERROR("Fail to read sector %lld.\n", start);
                return -1;
            }
            memcpy(extra_buf + offset - start, buf,
                   min(start +  SECTOR_SIZE - offset, len));
            if (cache->write_sector(start, extra_buf) != SECTOR_SIZE) {
                LOG_ERROR("Fail to write sector %lld\n", start);
                return -1;
            }
            write += min(start +  SECTOR_SIZE - offset, len);
        }

        if (j != end && (i == start || end - start > SECTOR_SIZE)) {
            if (cache->read_sector(end - SECTOR_SIZE, extra_buf)
                != SECTOR_SIZE)
            {
                LOG_ERROR("Fail to read sector %lld.", end - SECTOR_SIZE);
                return -1;
            }
            memcpy(extra_buf, (char*)buf + end - SECTOR_SIZE - offset,
                   offset + len + SECTOR_SIZE - end);
            if (cache->write_sector(end - SECTOR_SIZE, extra_buf)
                != SECTOR_SIZE)
            {
                LOG_ERROR("Fail to write sector %lld.", end - SECTOR_SIZE);
                return -1;
            }
            write += offset + len + SECTOR_SIZE - end;
        }

        for (u64 addr = i; addr < j; addr += SECTOR_SIZE) {
            if (cache->write_sector(addr, (char*)buf + addr - offset)
                != SECTOR_SIZE)
            {
                LOG_ERROR("Fail to write sector %lld.", addr);
                return -1;
            }
            write += SECTOR_SIZE;
        }
    } else {
        for (u64 addr = start; addr < end; addr += SECTOR_SIZE) {
            if (cache->write_sector(addr, (char *)buf + addr - start)
                != SECTOR_SIZE)
            {
                LOG_ERROR("Fail to write sector %lld.", addr);
                return -1;
            }
            write += SECTOR_SIZE;
        }
    }

    return write;
}

/* I/O control for target */
static ssize_t erofs_target_pread(struct erofs_vfile *vf, void *buf, u64 offset,
                                  size_t len)
{
    struct liberofs_file *target_file =
                            reinterpret_cast<struct liberofs_file *>(vf->ops);

    if (!target_file)
        return -EINVAL;
    if (erofs_read_photon_file(buf, offset, len, target_file->cache)
                               != (ssize_t)len)
        return -1;

    return len;
}

static ssize_t erofs_target_pwrite(struct erofs_vfile *vf, const void *buf,
                                   u64 offset, size_t len)
{
    struct liberofs_file *target_file =
                            reinterpret_cast<struct liberofs_file *>(vf->ops);

    if (!target_file)
        return -EINVAL;
    if (!buf)
        return -EINVAL;

    return erofs_write_photon_file(buf, offset, len, target_file->cache);
}

static int erofs_target_fsync(struct erofs_vfile *vf)
{
    struct liberofs_file *target_file =
                            reinterpret_cast<struct liberofs_file *>(vf->ops);

    if (!target_file)
        return -EINVAL;
    return target_file->cache->flush();
}

static int erofs_target_fallocate(struct erofs_vfile *vf, u64 offset,
                                  size_t len, bool pad)
{
	static const char zero[4096] = {0};
	ssize_t ret;

	while (len > 4096) {
		ret = erofs_target_pwrite(vf, zero, offset, 4096);
		if (ret)
			return ret;
		len -= 4096;
		offset += 4096;
	}
	ret = erofs_target_pwrite(vf, zero, offset, len);
	if (ret != (ssize_t)len) {
		return -1;
	}
	return 0;
}

static int erofs_target_ftruncate(struct erofs_vfile *vf, u64 length)
{
    return 0;
}

static ssize_t erofs_target_read(struct erofs_vfile *vf, void *buf, size_t len)
{
    return -EROFS_UNIMPLEMENTED;
}

static off_t erofs_target_lseek(struct erofs_vfile *vf, u64 offset, int whence)
{
    return -EROFS_UNIMPLEMENTED;
}

/* I/O control for source */
static ssize_t erofs_source_pread(struct erofs_vfile *vf, void *buf, u64 offset,
                                  size_t len)
{
    return -EROFS_UNIMPLEMENTED;
}

static ssize_t erofs_source_pwrite(struct erofs_vfile *vf, const void *buf,
                                   u64 offset, size_t len)
{
    return -EROFS_UNIMPLEMENTED;
}

static int erofs_source_fsync(struct erofs_vfile *vf)
{
    return -EROFS_UNIMPLEMENTED;
}

static int erofs_source_fallocate(struct erofs_vfile *vf,
                                  u64 offset, size_t len, bool pad)
{
    return -EROFS_UNIMPLEMENTED;
}

static int erofs_source_ftruncate(struct erofs_vfile *vf, u64 length)
{
    return -EROFS_UNIMPLEMENTED;
}

static ssize_t erofs_source_read(struct erofs_vfile *vf, void *buf,
                                 size_t bytes)
{
    struct liberofs_file *source_file =
                            reinterpret_cast<struct liberofs_file *>(vf->ops);

    if (!source_file)
        return -EINVAL;
    return source_file->file->read(buf, bytes);
}

static off_t erofs_source_lseek(struct erofs_vfile *vf, u64 offset, int whence)
{
    struct liberofs_file *source_file =
                            reinterpret_cast<struct liberofs_file *>(vf->ops);
    if (!source_file)
        return -EINVAL;
    return source_file->file->lseek(offset, whence);
}

struct erofs_mkfs_cfg {
    struct erofs_sb_info *sbi;
    struct erofs_tarfile *erofstar;
    bool incremental;
    bool ovlfs_strip;
    FILE *mp_fp;
};

static int rebuild_src_count;

int erofs_mkfs(struct erofs_mkfs_cfg *mkfs_cfg)
{
    int err;
    struct erofs_tarfile *erofstar;
    struct erofs_sb_info *sbi;
    struct erofs_buffer_head *sb_bh;
    struct erofs_inode *root = NULL;
    erofs_blk_t nblocks;

    erofstar = mkfs_cfg->erofstar;
    sbi = mkfs_cfg->sbi;
    if (!erofstar || !sbi)
        return -EINVAL;

    if (!mkfs_cfg->mp_fp)
        return -EINVAL;

    err = erofs_blocklist_open(mkfs_cfg->mp_fp, true);
    if (err) {
        LOG_ERROR("[erofs] Fail to open erofs blocklist.");
        return -EINVAL;
    }

    if (!mkfs_cfg->incremental) {
        sbi->bmgr = erofs_buffer_init(sbi, 0);
        if (!sbi->bmgr) {
            err = -ENOMEM;
            goto exit;
        }
        sb_bh = erofs_reserve_sb(sbi->bmgr);
        if (IS_ERR(sb_bh)) {
            LOG_ERROR("[erofs] Fail to reseve space for superblock.");
            err =  PTR_ERR(sb_bh);
            goto exit;
        }
    } else {
        err = erofs_read_superblock(sbi);
        if (err) {
            LOG_ERROR("[erofs] Fail to read superblock.");
            goto exit;
        }
        sbi->bmgr = erofs_buffer_init(sbi, sbi->primarydevice_blocks);
        if (!sbi->bmgr) {
            err = -ENOMEM;
            goto exit;
        }
        sb_bh = NULL;
    }

    erofs_inode_manager_init();

    root = erofs_rebuild_make_root(sbi);
    if (IS_ERR(root)) {
        LOG_ERROR("[erofs] Fail to alloc root inode.");
        err = PTR_ERR(root);
        goto exit;
    }

    while (!(err = tarerofs_parse_tar(root, erofstar)));
    if (err < 0) {
        LOG_ERROR("[erofs] Fail to parse tar file.", err);
        goto exit;
    }

    err = erofs_rebuild_dump_tree(root, mkfs_cfg->incremental);
    if (err < 0) {
        LOG_ERROR("[erofs] Fail to dump tree.", err);
        goto exit;
    }

    if (!erofstar->rvsp_mode) {
       err = erofs_mkfs_dump_blobs(sbi);
       if (err) {
           LOG_ERROR("[erofs] Fail to dump blob", err);
           goto exit;
       }
    }

    err = erofs_bflush(sbi->bmgr, NULL);
    if (err) {
        LOG_ERROR("[erofs] Bflush failed.");
        goto exit;
    }

    erofs_fixup_root_inode(root);
    erofs_iput(root);
    root = NULL;

    err = erofs_writesb(sbi, sb_bh, &nblocks);
    if (err) {
        LOG_ERROR("[erofs] Fail to writesb");
        goto exit;
    }

    /* flush all remaining buffers */
    err = erofs_bflush(sbi->bmgr, NULL);
    if (err)
        goto exit;

    err = erofs_dev_resize(sbi, nblocks);
exit:
    if (root)
        erofs_iput(root);
    erofs_buffer_exit(sbi->bmgr);
    erofs_blocklist_close();
    return err;
}

static int erofs_init_sbi(struct erofs_sb_info *sbi, photon::fs::IFile *fout,
                          struct erofs_vfops *ops, int blkbits)
{
    sbi->blkszbits = (char)blkbits;
    sbi->bdev.ops = ops;
    fout->lseek(0, 0);
    sbi->devsz = INT64_MAX;

    return 0;
}

static int erofs_init_tar(struct erofs_tarfile *erofstar,
                          struct erofs_vfops *ops)
{
    erofstar->global.xattrs = LIST_HEAD_INIT(erofstar->global.xattrs);
    erofstar->aufs = true;
    erofstar->dev = rebuild_src_count + 1;

    erofstar->ios.feof = false;
    erofstar->ios.tail = erofstar->ios.head = 0;
    erofstar->ios.dumpfd = -1;
    erofstar->ios.sz = 0;
    erofstar->ios.bufsize = 16384;
    do {
            erofstar->ios.buffer = (char*)malloc(erofstar->ios.bufsize);
            if (erofstar->ios.buffer)
                    break;
            erofstar->ios.bufsize >>= 1;
    } while (erofstar->ios.bufsize >= 1024);

    if (!erofstar->ios.buffer)
            return -ENOMEM;

    erofstar->ios.vf.ops = ops;

    return 0;
}

static int erofs_write_map_file(photon::fs::IFile *fout, uint64_t blksz, FILE *fp)
{
    uint64_t blkaddr, toff;
    uint32_t nblocks, zeroedlen;
    char *line = NULL;
    size_t len  = 0;
    int cnt;

    if (fp == NULL) {
       LOG_ERROR("unable to get upper.map, ignored");
       return -1;
    }
    rewind(fp);

    while (getline(&line, &len, fp) != -1) {
        LSMT::RemoteMapping lba;

        cnt = sscanf(line, "%" PRIx64" %x%" PRIx64 "%u", &blkaddr, &nblocks, &toff, &zeroedlen);
        if (cnt < 3) {
            LOG_ERROR("Bad formatted map file.");
            break;
        }

        lba.offset = blkaddr * blksz;
        lba.count = nblocks * blksz;
        lba.roffset = toff;
        if (cnt > 3)
            lba.count = round_up_blk(lba.count - zeroedlen);

        int nwrite = fout->ioctl(LSMT::IFileRW::RemoteData, lba);
        if ((unsigned) nwrite != lba.count) {
            LOG_ERRNO_RETURN(0, -1, "failed to write lba");
        }
    }
    free(line);

    return 0;
}

static int erofs_close_sbi(struct erofs_sb_info *sbi, ErofsCache *cache)
{
    if (cache->flush()) {
        LOG_ERROR("Fail to flush caches.");
        return -1;
    }
    return 0;
}

static void erofs_close_tar(struct erofs_tarfile *erofstar)
{
    free(erofstar->ios.buffer);
}

int LibErofs::extract_tar(photon::fs::IFile *source, bool meta_only, bool first_layer)
{
    struct erofs_sb_info sbi = {};
    struct erofs_tarfile erofstar = {};
    struct erofs_mkfs_cfg mkfs_cfg;
    struct erofs_configure *erofs_cfg;
    struct liberofs_file target_file, source_file;
    int err, err2;

    target_file.ops.pread = erofs_target_pread;
    target_file.ops.pwrite = erofs_target_pwrite;
    target_file.ops.pread = erofs_target_pread;
    target_file.ops.pwrite = erofs_target_pwrite;
    target_file.ops.fsync = erofs_target_fsync;
    target_file.ops.fallocate = erofs_target_fallocate;
    target_file.ops.ftruncate = erofs_target_ftruncate;
    target_file.ops.read = erofs_target_read;
    target_file.ops.lseek = erofs_target_lseek;
    target_file.file = target;
    target_file.cache = new ErofsCache(target, 128);

    source_file.ops.pread = erofs_source_pread;
    source_file.ops.pwrite = erofs_source_pwrite;
    source_file.ops.fsync = erofs_source_fsync;
    source_file.ops.fallocate = erofs_source_fallocate;
    source_file.ops.ftruncate = erofs_source_ftruncate;
    source_file.ops.read = erofs_source_read;
    source_file.ops.lseek = erofs_source_lseek;
    source_file.file = source;
    source_file.cache = NULL;

    /* initialization of sbi */
    err = erofs_init_sbi(&sbi, target_file.file, &target_file.ops, ilog2(blksize));
    if (err) {
        erofs_close_sbi(&sbi, target_file.cache);
        delete target_file.cache;
        LOG_ERROR("Failed to init sbi.");
        return err;
    }
    /* initialization of erofstar */
    err = erofs_init_tar(&erofstar, &source_file.ops);
    if (err) {
        LOG_ERROR("Failed to init tarerofs");
        goto exit;
    }

    erofstar.rvsp_mode = true;
    if (ddtaridx)
            erofstar.ddtaridx_mode = true;
    mkfs_cfg.sbi = &sbi;
    mkfs_cfg.erofstar = &erofstar;
    mkfs_cfg.incremental = !first_layer;
    erofs_cfg = erofs_get_configure();
    erofs_cfg->c_ovlfs_strip = true;
    if (first_layer)
        erofs_cfg->c_root_xattr_isize = EROFS_ROOT_XATTR_SZ;
    else
        erofs_cfg->c_root_xattr_isize = 0;
    mkfs_cfg.mp_fp = std::tmpfile();

    err = erofs_mkfs(&mkfs_cfg);
    if (err) {
        LOG_ERROR("Failed to mkfs.");
        goto exit;
    }

    /* write mapfile */
    err = erofs_write_map_file(target_file.file, blksize, mkfs_cfg.mp_fp);
    if (err) {
        LOG_ERROR("Failed to write mapfile.");
        goto exit;
    }
exit:
    err2 = erofs_close_sbi(&sbi, target_file.cache);
    erofs_close_tar(&erofstar);
    std::fclose(mkfs_cfg.mp_fp);
    delete target_file.cache;
    return err ? err : err2;
}

LibErofs::LibErofs(photon::fs::IFile *target, uint64_t blksize, bool import_tar_headers)
    : target(target), blksize(blksize), ddtaridx(import_tar_headers)
{
}

LibErofs::~LibErofs()
{
}

class ErofsFileSystem: public photon::fs::IFileSystem {
public:
    struct erofs_sb_info sbi;
    struct liberofs_file target_file;

    ErofsFileSystem(photon::fs::IFile *imgfile, uint64_t blksize);
    ~ErofsFileSystem();
    photon::fs::IFile* open(const char *pathname, int flags);
    photon::fs::IFile* open(const char *pathname, int flags, mode_t mode);
    photon::fs::IFile* creat(const char *pathname, mode_t mode);
    int mkdir(const char *pathname, mode_t mode);
    int rmdir(const char *pathname);
    int symlink(const char *oldname, const char *newname);
    ssize_t readlink(const char *path, char *buf, size_t bufsiz);
    int link(const char *oldname, const char *newname);
    int rename(const char *oldname, const char *newname);
    int unlink(const char *filename);
    int chmod(const char *pathname, mode_t mode);
    int chown(const char *pathname, uid_t owner, gid_t group);
    int lchown(const char *pathname, uid_t owner, gid_t group);
    int statfs(const char *path, struct statfs *buf);
    int statvfs(const char *path, struct statvfs *buf);
    int stat(const char *path, struct stat *buf);
    int lstat(const char *path, struct stat *buf);
    int access(const char *pathname, int mode);
    int truncate(const char *path, off_t length);
    int utime(const char *path, const struct utimbuf *file_times);
    int utimes(const char *path, const struct timeval times[2]);
    int lutimes(const char *path, const struct timeval times[2]);
    int mknod(const char *path, mode_t mode, dev_t dev);
    int syncfs();
    photon::fs::DIR* opendir(const char *name);
};

class ErofsFile: public photon::fs::VirtualReadOnlyFile {
public:
    ErofsFileSystem *fs;
    struct erofs_inode inode;

    ErofsFile(ErofsFileSystem *fs);
    photon::fs::IFileSystem *filesystem();
    int fstat(struct stat *buf);
    int fiemap(struct photon::fs::fiemap *map);
};

class ErofsDir: public photon::fs::DIR {
public:
    std::vector<::dirent> m_dirs;
    ::dirent *direntp = nullptr;
    long loc;
    ErofsDir(std::vector<::dirent> &dirs);
    ~ErofsDir();
    int closedir();
    dirent *get();
    int next();
    void rewinddir();
    void seekdir(long loc);
    long telldir();
};

#define EROFS_UNIMPLEMENTED_FUNC(ret_type, cls, func, ret) \
ret_type cls::func { \
    return ret; \
}

// ErofsFile
ErofsFile::ErofsFile(ErofsFileSystem *fs): fs(fs)
{
    memset(&inode, 0, sizeof(struct erofs_inode));
}
photon::fs::IFileSystem *ErofsFile::filesystem() { return fs; }

struct liberofs_nameidata {
    struct erofs_sb_info *sbi;
	erofs_nid_t	nid;
};

static int liberofs_link_path_walk(const char *name,
                                   struct liberofs_nameidata *nd);

static struct erofs_dirent *liberofs_find_dirent(void *data, const char *name,
                                                unsigned int len,
                                                unsigned int nameoff,
                                                unsigned int maxsize)
{
    struct erofs_dirent *de = (struct erofs_dirent *)data;
    const struct erofs_dirent *end = (struct erofs_dirent *)((char *)data + nameoff);

    while (de < end) {
        const char *de_name;
        unsigned int de_namelen;

        nameoff = le16_to_cpu(de->nameoff);
        de_name = (char *)((char *)data + nameoff);
        if (de + 1 >= end)
            de_namelen = strnlen(de_name, maxsize - nameoff);
        else
            de_namelen = le16_to_cpu(de[1].nameoff - nameoff);

        if (nameoff + de_namelen > maxsize) {
            LOG_ERROR("[erofs] bogus dirent");
            return (struct erofs_dirent *)ERR_PTR(-EINVAL);
        }

        if (len == de_namelen && !memcmp(de_name, name, de_namelen))
            return de;
        ++de;
    }
    return NULL;
}

static int liberofs_namei(struct liberofs_nameidata *nd, const char *name,
                          unsigned int len)
{
	erofs_nid_t nid = nd->nid;
	int ret;
	char buf[EROFS_MAX_BLOCK_SIZE];
	struct erofs_sb_info *sbi = nd->sbi;
	struct erofs_inode vi = {};
	erofs_off_t offset;

	vi.sbi = sbi;
	vi.nid = nid;
	ret = erofs_read_inode_from_disk(&vi);
	if (ret)
		return ret;

	offset = 0;
	while (offset < vi.i_size) {
		erofs_off_t maxsize = min_t(erofs_off_t,
					    vi.i_size - offset, erofs_blksiz(sbi));
		struct erofs_dirent *de = (struct erofs_dirent *)buf;
		unsigned int nameoff;

		ret = erofs_pread(&vi, buf, maxsize, offset);
		if (ret)
			return ret;

		nameoff = le16_to_cpu(de->nameoff);
		if (nameoff < sizeof(struct erofs_dirent) ||
            nameoff >= erofs_blksiz(sbi))
                LOG_ERRNO_RETURN(-EINVAL, -EINVAL, "[erofs] invalid nameoff");

		de = liberofs_find_dirent(buf, name, len, nameoff, maxsize);
		if (IS_ERR(de))
			return PTR_ERR(de);

		if (de) {
			nd->nid = le64_to_cpu(de->nid);
			return 0;
		}
		offset += maxsize;
	}
	return -ENOENT;
}


static int liberofs_step_into_link(struct liberofs_nameidata *nd,
                                   struct erofs_inode *vi)
{
    char buf[PATH_MAX];
    int err;

    if (vi->i_size > PATH_MAX)
        return -EINVAL;
    memset(buf, 0, sizeof(buf));
    err = erofs_pread(vi, buf, vi->i_size, 0);
    if (err)
        return err;
    return liberofs_link_path_walk(buf, nd);
}

static int liberofs_link_path_walk(const char *name,
                                   struct liberofs_nameidata *nd)
{
    struct erofs_inode vi;
    erofs_nid_t nid;
    const char *p;
    int ret;

    if (*name == '/')
        nd->nid = nd->sbi->root_nid;

    while (*name == '/')
        name ++;

    while (*name != '\0') {
        p = name;
        do {
            ++p;
        } while (*p != '\0' && *p != '/');

        nid = nd->nid;
        ret = liberofs_namei(nd, name, p - name);
        if (ret)
            return ret;
        vi.sbi = nd->sbi;
        vi.nid = nd->nid;
        ret = erofs_read_inode_from_disk(&vi);
        if (ret)
            return ret;
        if (S_ISLNK(vi.i_mode)) {
            nd->nid = nid;
            ret = liberofs_step_into_link(nd, &vi);
            if (ret)
                return ret;
        }
        for (name = p; *name == '/'; ++name)
            ;
    }
    return 0;
}


static int do_erofs_ilookup(const char *path, struct erofs_inode *vi)
{
    int ret;
    struct liberofs_nameidata nd = {.sbi = vi->sbi};

    nd.nid = vi->sbi->root_nid;
    ret = liberofs_link_path_walk(path, &nd);
    if (ret)
        return ret;
    vi->nid = nd.nid;
    return erofs_read_inode_from_disk(vi);
}

int ErofsFile::fstat(struct stat *buf)
{
    buf->st_mode = inode.i_mode;
    buf->st_nlink = inode.i_nlink;
    buf->st_size = inode.i_size;
    buf->st_blocks = roundup(inode.i_size, erofs_blksiz(inode.sbi)) >> 9;
    buf->st_uid = inode.i_uid;
    buf->st_gid = inode.i_gid;
    buf->st_ctime = inode.i_mtime;
    buf->st_mtime = inode.i_mtime;
    buf->st_atime = inode.i_mtime;
    return 0;
}

int ErofsFile::fiemap(struct photon::fs::fiemap *map)
{
    photon::fs::fiemap_extent *ext_buf = &map->fm_extents[0];
    struct erofs_map_blocks erofs_map;
    int err;

    map->fm_mapped_extents = 0;
    erofs_map.index = UINT_MAX;
    erofs_map.m_la = 0;

    while (erofs_map.m_la < inode.i_size) {
        err = erofs_map_blocks(&inode, &erofs_map, 0);
        if (err)
            LOG_ERROR_RETURN(err, err, "[erofs] Fail to map erofs blocks");
        ext_buf[map->fm_mapped_extents].fe_physical = erofs_map.m_pa;
        ext_buf[map->fm_mapped_extents].fe_length = erofs_map.m_plen;
        map->fm_mapped_extents += 1;
        erofs_map.m_la += erofs_map.m_llen;
    }
    return 0;
}

// ErofsFileSystem
EROFS_UNIMPLEMENTED_FUNC(photon::fs::IFile*, ErofsFileSystem, open(const char *pathname, int flags, mode_t mode), NULL)
EROFS_UNIMPLEMENTED_FUNC(photon::fs::IFile*, ErofsFileSystem, creat(const char *pathname, mode_t mode), NULL)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, mkdir(const char *pathname, mode_t mode), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, rmdir(const char *pathname), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, symlink(const char *oldname, const char *newname), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(ssize_t, ErofsFileSystem, readlink(const char *path, char *buf, size_t bufsiz), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, link(const char *oldname, const char *newname), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, rename(const char *oldname, const char *newname), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, unlink(const char *filename), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, chmod(const char *pathname, mode_t mode), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, chown(const char *pathname, uid_t owner, gid_t group), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, lchown(const char *pathname, uid_t owner, gid_t group), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, statfs(const char *path, struct statfs *buf), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, statvfs(const char *path, struct statvfs *buf), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, lstat(const char *path, struct stat *buf), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, access(const char *pathname, int mode), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, truncate(const char *path, off_t length), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, utime(const char *path, const struct utimbuf *file_times), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, utimes(const char *path, const struct timeval times[2]), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, lutimes(const char *path, const struct timeval times[2]), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, mknod(const char *path, mode_t mode, dev_t dev), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, syncfs(), -EROFS_UNIMPLEMENTED)

ErofsFileSystem::ErofsFileSystem(photon::fs::IFile *imgfile, uint64_t blksize)
{
    target_file.ops.pread = erofs_target_pread;
    target_file.ops.pwrite = erofs_target_pwrite;
    target_file.ops.pread = erofs_target_pread;
    target_file.ops.pwrite = erofs_target_pwrite;
    target_file.ops.fsync = erofs_target_fsync;
    target_file.ops.fallocate = erofs_target_fallocate;
    target_file.ops.ftruncate = erofs_target_ftruncate;
    target_file.ops.read = erofs_target_read;
    target_file.ops.lseek = erofs_target_lseek;
    target_file.file = imgfile;
    target_file.cache = new ErofsCache(target_file.file, 128);

    memset(&sbi, 0, sizeof(struct erofs_sb_info));
    (void)erofs_init_sbi(&sbi, target_file.file, &target_file.ops, ilog2(blksize));
    if (erofs_read_superblock(&sbi))
        LOG_ERROR("[erofs] Fail to read_super_block");
}

ErofsFileSystem::~ErofsFileSystem()
{
    delete target_file.cache;
}

int ErofsFileSystem::stat(const char *path, struct stat *buf)
{
    struct erofs_inode vi;
    int err;

    vi.sbi = &sbi;
    err = do_erofs_ilookup(path, &vi);
    if (err)
        LOG_ERRNO_RETURN(err, err, "[erofs] Fail to lookup inode");
    buf->st_mode = vi.i_mode;
    buf->st_nlink = vi.i_nlink;
    buf->st_size = vi.i_size;
    buf->st_blocks = roundup(vi.i_size, erofs_blksiz(vi.sbi)) >> 9;
    buf->st_uid = vi.i_uid;
    buf->st_gid = vi.i_gid;
    buf->st_ctime = vi.i_mtime;
    buf->st_mtime = vi.i_mtime;
    buf->st_atime = vi.i_mtime;
    return 0;
}

photon::fs::IFile* ErofsFileSystem::open(const char *pathname, int flags)
{
    ErofsFile *file = new ErofsFile(this);
    int err;

    file->inode.sbi = &sbi;
    err = do_erofs_ilookup(pathname, &file->inode);
    if (err) {
        delete file;
        LOG_ERROR_RETURN(-err, nullptr, "[erofs] Fail to lookup inode by path");
    }
    return file;
}

struct liberofs_dir_context {
    struct erofs_dir_context ctx;
    std::vector<::dirent> *dirs;
};

static int liberofs_readdir(struct erofs_dir_context *ctx)
{
    struct liberofs_dir_context *libctx = reinterpret_cast<struct liberofs_dir_context *>(ctx);
    std::vector<::dirent> *dirs  = libctx->dirs;
    struct dirent tmpdir;

    if (ctx->dot_dotdot)
        return 0;

    tmpdir.d_ino = (ino_t) ctx->de_nid;
    tmpdir.d_off = 0;
    tmpdir.d_reclen = sizeof(struct erofs_dirent);
    if (ctx->de_namelen > sizeof(tmpdir.d_name))
        LOG_ERROR_RETURN(-EINVAL, -EINVAL, "[erofs] Invalid name length");
    memset(tmpdir.d_name, 0, sizeof(tmpdir.d_name));
    memcpy(tmpdir.d_name, ctx->dname, ctx->de_namelen);
    dirs->emplace_back(tmpdir);
    return 0;
}

static int do_erofs_readdir(struct erofs_sb_info *sbi, const char *path,  std::vector<::dirent> *dirs)
{
    struct liberofs_dir_context ctx;
    struct erofs_inode vi;
    int err;

    vi.sbi = sbi;
    err = do_erofs_ilookup(path, &vi);
    if (err)
        LOG_ERRNO_RETURN(err, err, "[erofs] Fail to lookup inode");
    ctx.ctx.dir = &vi;
    ctx.ctx.cb = liberofs_readdir;
    ctx.dirs = dirs;

    return erofs_iterate_dir(&ctx.ctx, false);
}

photon::fs::DIR* ErofsFileSystem::opendir(const char *name)
{
    std::vector<::dirent> dirs;

    auto ret = do_erofs_readdir(&sbi, name, &dirs);
    if (ret) {
        errno = -ret;
        return nullptr;
    }
    return new ErofsDir(dirs);
}


// ErofsDir
ErofsDir::ErofsDir(std::vector<::dirent> &dirs) : loc(0) {
        m_dirs = std::move(dirs);
        next();
}

ErofsDir::~ErofsDir() {
    closedir();
}

int ErofsDir::closedir() {
    if (!m_dirs.empty()) {
        m_dirs.clear();
    }
    return 0;
}

dirent *ErofsDir::get() {
    return direntp;
}

int ErofsDir::next() {
    if (!m_dirs.empty()) {
        if (loc < (long) m_dirs.size()) {
            direntp = &m_dirs[loc++];
        } else {
            direntp = nullptr;
        }
    }
    return direntp != nullptr ? 1 : 0;
}

void ErofsDir::rewinddir() {
    loc = 0;
    next();
}

void ErofsDir::seekdir(long loc){
    this->loc = loc;
    next();
}

long ErofsDir::telldir() {
    return loc;
}

bool erofs_check_fs(const photon::fs::IFile *imgfile)
{
	u8 data[EROFS_MAX_BLOCK_SIZE];
	struct erofs_super_block *dsb;
	photon::fs::IFile *file = const_cast<photon::fs::IFile *>(imgfile);
	int ret;

	ret = file->pread(data, EROFS_MAX_BLOCK_SIZE, 0);
	if (ret != EROFS_MAX_BLOCK_SIZE)
		LOG_ERROR_RETURN(-EIO, false, "[erofs] Fail to read superblock");
	dsb = reinterpret_cast<struct erofs_super_block *>(data + EROFS_SUPER_OFFSET);
	return le32_to_cpu(dsb->magic) == EROFS_SUPER_MAGIC_V1;
}

photon::fs::IFileSystem *erofs_create_fs(photon::fs::IFile *imgfile, uint64_t blksz)
{
    return new ErofsFileSystem(imgfile, blksz);
}
