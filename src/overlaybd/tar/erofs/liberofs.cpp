#include "liberofs.h"
#include "erofs/tar.h"
#include "erofs/io.h"
#include "erofs/cache.h"
#include "erofs/blobchunk.h"
#include "erofs/block_list.h"
#include "erofs/inode.h"
#include "erofs/config.h"
#include "../../lsmt/file.h"
#include "../../lsmt/index.h"
#include <photon/common/alog.h>
#include <map>
#include <set>

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

int erofs_mkfs(struct erofs_mkfs_cfg *cfg)
{
    int err;
    struct erofs_tarfile *erofstar;
    struct erofs_sb_info *sbi;
    struct erofs_buffer_head *sb_bh;
    struct erofs_inode *root = NULL;
    erofs_blk_t nblocks;

    erofstar = cfg->erofstar;
    sbi = cfg->sbi;
    if (!erofstar || !sbi)
        return -EINVAL;

    if (!cfg->mp_fp)
        return -EINVAL;

    err = erofs_blocklist_open(cfg->mp_fp, true);
    if (err) {
        LOG_ERROR("[erofs] Fail to open erofs blocklist.");
        return -EINVAL;
    }

    if (!cfg->incremental) {
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

    err = erofs_rebuild_dump_tree(root, cfg->incremental);
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
    uint32_t nblocks;

    if (fp == NULL) {
       LOG_ERROR("unable to get upper.map, ignored");
       return -1;
    }
    rewind(fp);
    while (fscanf(fp, "%" PRIx64" %x %" PRIx64 "\n", &blkaddr, &nblocks, &toff)
           >= 3)
    {
        LSMT::RemoteMapping lba;
        lba.offset = blkaddr * blksz;
        lba.count = nblocks * blksz;
        lba.roffset = toff;
        int nwrite = fout->ioctl(LSMT::IFileRW::RemoteData, lba);
        if ((unsigned) nwrite != lba.count) {
            LOG_ERRNO_RETURN(0, -1, "failed to write lba");
        }
    }

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
    struct erofs_mkfs_cfg cfg;
    struct erofs_configure *erofs_cfg;
    struct liberofs_file target_file, source_file;
    int err;

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
    cfg.sbi = &sbi;
    cfg.erofstar = &erofstar;
    cfg.incremental = !first_layer;
    erofs_cfg = erofs_get_configure();
    erofs_cfg->c_ovlfs_strip = true;
    if (first_layer)
        erofs_cfg->c_root_xattr_isize = EROFS_ROOT_XATTR_SZ;
    else
        erofs_cfg->c_root_xattr_isize = 0;
    cfg.mp_fp = std::tmpfile();

    err = erofs_mkfs(&cfg);
    if (err) {
        LOG_ERROR("Failed to mkfs.");
        goto exit;
    }

    /* write mapfile */
    err = erofs_write_map_file(target_file.file, blksize, cfg.mp_fp);
    if (err) {
        LOG_ERROR("Failed to write mapfile.");
        goto exit;
    }
exit:
    err = erofs_close_sbi(&sbi, target_file.cache);
    erofs_close_tar(&erofstar);
    std::fclose(cfg.mp_fp);
    delete target_file.cache;
    return err;
}

LibErofs::LibErofs(photon::fs::IFile *target, uint64_t blksize, bool import_tar_headers)
    : target(target), blksize(blksize), ddtaridx(import_tar_headers)
{
}

LibErofs::~LibErofs()
{
}
