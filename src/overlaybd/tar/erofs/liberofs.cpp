#include "erofs_common.h"
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

struct erofs_mkfs_cfg {
    struct erofs_sb_info *sbi;
    struct erofs_tarfile *erofstar;
    bool incremental;
    bool ovlfs_strip;
    FILE *mp_fp;
};

static int rebuild_src_count;

extern void erofs_init_configure(void);

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
    int cnt, err = 0;

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
            err = -EINVAL;
            break;
        }

        lba.offset = blkaddr * blksz;
        lba.count = nblocks * blksz;
        lba.roffset = toff;
        if (cnt > 3)
            lba.count = round_up_blk(lba.count - zeroedlen);

        int nwrite = fout->ioctl(LSMT::IFileRW::RemoteData, lba);
        if ((unsigned) nwrite != lba.count) {
            LOG_ERROR("failed to write lba");
            err = -EINVAL;
            break;
        }
    }
    if (line)
        free(line);

    return err;
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
    erofs_init_configure();
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
