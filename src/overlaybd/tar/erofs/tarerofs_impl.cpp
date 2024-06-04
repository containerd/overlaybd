#include "tarerofs_interface.h"
#include "tarerofs_impl.h"
#include "erofs/tar.h"
#include "erofs/io.h"
#include "erofs/cache.h"
#include "erofs/block_list.h"
#include "erofs/inode.h"
#include "../../lsmt/file.h"
#include "../../lsmt/index.h"
#include <photon/common/alog.h>

#define TAREROFS_BLOCK_SIZE 4096
#define TAREROFS_BLOCK_BITS 12
#define DATA_OFFSET 1073741824
#define MIN_RW_LEN 512ULL
#define round_down_blk(addr) ((addr) & (~(MIN_RW_LEN - 1)))
#define round_up_blk(addr) (round_down_blk((addr) + MIN_RW_LEN - 1))
#define MAP_FILE_NAME "upper.map"

/*
 * Helper function for reading from the photon file, since
 * the photon file requires reads to be 512-byte aligned.
 */
static ssize_t read_photon_file(void *buf, u64 offset, size_t len, photon::fs::IFile *file)
{
    size_t ret;
    u64 start, end;
    char *big_buf;

    start = round_down_blk(offset);
    end = round_up_blk(offset + len);

    big_buf = (char *)malloc(end - start);
    if (!big_buf) {
        LOG_ERROR("Fail to malloc.");
        return -1;
    }

    ret = file->pread(big_buf, end - start, start);
    if (ret != end - start) {
        LOG_ERROR("Pread faild.");
        free(big_buf);
        return -1;
    }

    memcpy(buf, big_buf + offset - start, len);
    return len;
}

/*
 * Helper function for writing to a photon file.
 */
 static ssize_t write_photon_file(const void *buf, u64 offset, size_t len, photon::fs::IFile *file)
 {
    ssize_t ret;
    u64 start, end;
    size_t saved_len = len;
    char *big_buf;

    start = round_down_blk(offset);
    end = round_up_blk(offset + len);

    if (start != offset || end != offset + len) {
        big_buf = (char *)malloc(end - start);
        if (!big_buf) {
            LOG_ERROR("Fail to malloc.");
            return -1;
        }
        /* writes within a sector range */
        if (end - start == MIN_RW_LEN) {
            ret = file->pread(big_buf, MIN_RW_LEN, start);
            if (ret != MIN_RW_LEN) {
                LOG_ERROR("Fail to pread.");
                free(big_buf);
                return -1;
            }
        } else {
            /*
             * writes that span at least two sectors,
             * we read the head and tail sectors in such case
             */
            if (start != offset) {
                ret = file->pread(big_buf, (size_t)MIN_RW_LEN, start);
                if (ret != MIN_RW_LEN) {
                    LOG_ERROR("Fail to pread.");
                    free(big_buf);
                    return -1;
                }
            }

            if (end != offset + len) {
                ret = file->pread(big_buf + end - start - MIN_RW_LEN,
                                (size_t)MIN_RW_LEN,
                                (off_t)end - MIN_RW_LEN);
                if (ret != MIN_RW_LEN) {
                    LOG_ERROR("Fail to pread.");
                    free(big_buf);
                    return -1;
                }
            }
        }

        memcpy(big_buf + offset - start, buf, len);
        len = end - start;
        ret = file->pwrite(big_buf, len, start);
        free(big_buf);
    } else {
        ret = file->pwrite(buf, len, offset);
    }

    if ((size_t)ret != len) {
        LOG_ERROR("Fail to write photo file.");
        return -1;
    }

    return saved_len;
}

TarErofsInter::TarErofsImpl *TarErofsInter::TarErofsImpl::ops_to_tarerofsimpl(struct erofs_vfops *ops)
{
    struct erofs_vfops_wrapper *evw = reinterpret_cast<struct erofs_vfops_wrapper*>(ops);
    TarErofsImpl *obj = reinterpret_cast<TarErofsImpl*>(evw->private_data);
    return obj;
}

/* I/O control for target */
ssize_t TarErofsInter::TarErofsImpl::target_pread(struct erofs_vfile *vf, void *buf, u64 offset, size_t len)
{
    TarErofsImpl *obj = ops_to_tarerofsimpl(vf->ops);
    photon::fs::IFile *fout = obj->fout;

    if (read_photon_file(buf, offset, len, fout) != (ssize_t)len)
        return -1;
    return len;
}

ssize_t TarErofsInter::TarErofsImpl::target_pwrite(struct erofs_vfile *vf, const void *buf, u64 offset, size_t len)
{
    TarErofsImpl *obj = ops_to_tarerofsimpl(vf->ops);
    photon::fs::IFile *fout = obj->fout;
    ssize_t ret;

    if (!buf)
        return -EINVAL;

    ret = write_photon_file(buf, offset, len, fout);
    return ret;
}

int TarErofsInter::TarErofsImpl::target_fsync(struct erofs_vfile *vf)
{
    TarErofsImpl *obj = ops_to_tarerofsimpl(vf->ops);
    photon::fs::IFile *fout = obj->fout;

    return fout->fsync();
}

int TarErofsInter::TarErofsImpl::target_fallocate(struct erofs_vfile *vf, u64 offset, size_t len, bool pad)
{
	static const char zero[4096] = {0};
	ssize_t ret;

	while (len > 4096) {
		ret = target_pwrite(vf, zero, offset, 4096);
		if (ret)
			return ret;
		len -= 4096;
		offset += 4096;
	}
	ret = target_pwrite(vf, zero, offset, len);
	if (ret != (ssize_t)len) {
		return -2;
	}
	return 0;
}

int TarErofsInter::TarErofsImpl::target_ftruncate(struct erofs_vfile *vf, u64 length)
{
    return 0;
}


ssize_t TarErofsInter::TarErofsImpl::target_read(struct erofs_vfile *vf, void *buf, size_t len)
{
    return -1;
}

off_t TarErofsInter::TarErofsImpl::target_lseek(struct erofs_vfile *vf, u64 offset, int whence)
{
    return -1;
}

/* I/O control for source */
ssize_t TarErofsInter::TarErofsImpl::source_pread(struct erofs_vfile *vf, void *buf, u64 offset, size_t len)
{
    return -1;
}

ssize_t TarErofsInter::TarErofsImpl::source_pwrite(struct erofs_vfile *vf, const void *buf, u64 offset, size_t len)
{
    return -1;
}

int TarErofsInter::TarErofsImpl::source_fsync(struct erofs_vfile *vf)
{
    return -1;
}

int TarErofsInter::TarErofsImpl::source_fallocate(struct erofs_vfile *vf, u64 offset, size_t len, bool pad)
{
    return -1;
}

int TarErofsInter::TarErofsImpl::source_ftruncate(struct erofs_vfile *vf, u64 length)
{
    return -1;
}


ssize_t TarErofsInter::TarErofsImpl::source_read(struct erofs_vfile *vf, void *buf, size_t bytes)
{
    TarErofsImpl *obj = ops_to_tarerofsimpl(vf->ops);

    u64 i = 0;
    while (bytes) {
        u64 len = bytes > INT_MAX ? INT_MAX : bytes;
        u64 ret;

        ret = obj->file->read(buf + i, len);
        if (ret < 1) {
            if (ret == 0)
                break;
            else
                return -1;
        }
        bytes -= ret;
        i += ret;
    }
    return i;
}


off_t TarErofsInter::TarErofsImpl::source_lseek(struct erofs_vfile *vf, u64 offset, int whence)
{
    TarErofsImpl *obj = ops_to_tarerofsimpl(vf->ops);
    photon::fs::IFile *file = obj->file;

    return file->lseek(offset, whence);
}


struct erofs_mkfs_cfg {
    struct erofs_sb_info *sbi;
    struct erofs_tarfile *erofstar;
    bool incremental;
    bool ovlfs_strip;
};

static int rebuild_src_count;

int erofs_mkfs(struct erofs_mkfs_cfg *cfg) {
    int err;
    struct erofs_tarfile *erofstar;
    struct erofs_sb_info *sbi;
    struct erofs_buffer_head *sb_bh;
    struct erofs_inode *root;
    erofs_blk_t nblocks;

    erofstar = cfg->erofstar;
    sbi = cfg->sbi;
    if (!erofstar || !sbi)
        return -EINVAL;

    if (!erofstar->mapfile)
        return -EINVAL;


    err = erofs_blocklist_open(erofstar->mapfile, true);
    if (err) {
        LOG_ERROR("[erofs] Fail to open erofs blocklist.");
        return -EINVAL;
    }

    if (!erofstar->rvsp_mode) {
        LOG_ERROR("[erofs] Must be in RVSP mode.");
        return -EINVAL;
    }

    if (!cfg->incremental) {
        sb_bh = erofs_reserve_sb(sbi);
        if (IS_ERR(sb_bh)) {
            LOG_ERROR("[erofs] Fail to reseve space for superblock.");
            err =  PTR_ERR(sb_bh);
            goto exit;
        }
        //erofs_uuid_generate(sbi->uuid);
    } else {
        err = erofs_read_superblock(sbi);
        if (err) {
            LOG_ERROR("[erofs] Fail to read superblock.");
            goto exit;
        }
        erofs_buffer_init(sbi, sbi->primarydevice_blocks);
        sb_bh = NULL;
    }

    erofs_inode_manager_init();

    root = erofs_mkfs_alloc_root(sbi);
    if (IS_ERR(root)) {
        LOG_ERROR("[erofs] Fail to alloc root inode.");
        err = PTR_ERR(root);
        goto exit;
    }

    while (!(err = tarerofs_parse_tar(root, erofstar)));
    if (err < 0) {
        LOG_ERROR("[erofs] Fail to parse tar file.");
        goto exit;
    }

    err = erofs_rebuild_dump_tree(root, cfg->incremental, cfg->ovlfs_strip);
    if (err < 0) {
        LOG_ERROR("[erofs] Fail to dump tree.");
        goto exit;
    }

    err = erofs_bflush(NULL);
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

    err = erofs_dev_resize(sbi, nblocks);
exit:
    if (root)
        erofs_iput(root);
    erofs_blocklist_close();
    return err;
}

static int init_sbi(struct erofs_sb_info *sbi, photon::fs::IFile *fout, struct erofs_vfops *ops)
{
    int err;
    struct timeval t;

    sbi->blkszbits = TAREROFS_BLOCK_BITS;
    err = gettimeofday(&t, NULL);
    if (err)
        return err;
    sbi->build_time = t.tv_sec;
    sbi->build_time_nsec = t.tv_usec;
    sbi->bdev.ops = ops;
    fout->lseek(0, 0);
    sbi->devsz = INT64_MAX;

    return 0;
}

static int init_tar(struct erofs_tarfile *erofstar, photon::fs::IFile *tar_file, struct erofs_vfops *ops)
{
    int err;
    struct stat st;

    erofstar->global.xattrs = LIST_HEAD_INIT(erofstar->global.xattrs);
    erofstar->mapfile = MAP_FILE_NAME;
    erofstar->aufs = true;
    erofstar->rvsp_mode = true;
    erofstar->dev = rebuild_src_count + 1;

    erofstar->ios.feof = false;
    erofstar->ios.tail = erofstar->ios.head = 0;
    erofstar->ios.dumpfd = -1;
    err = tar_file->fstat(&st);
    if (err) {
        LOG_ERROR("Fail to fstat tar file.");
        return err;
    }
    erofstar->ios.sz = st.st_size;
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

static int write_map_file(photon::fs::IFile *fout)
{
    FILE *fp;
    uint64_t blkaddr, toff;
    uint32_t nblocks;

    fp = fopen(MAP_FILE_NAME, "r");
    if (fp == NULL) {
       LOG_ERROR("unable to get upper.map, ignored");
       return -1;
    }

    while (fscanf(fp, "%" PRIx64" %x %" PRIx64 "\n", &blkaddr, &nblocks, &toff) >= 3) {
        LSMT::RemoteMapping lba;
        lba.offset = blkaddr * TAREROFS_BLOCK_SIZE;
        lba.count = nblocks * TAREROFS_BLOCK_SIZE;
        lba.roffset = toff;
        int nwrite = fout->ioctl(LSMT::IFileRW::RemoteData, lba);
        if ((unsigned) nwrite != lba.count) {
            LOG_ERRNO_RETURN(0, -1, "failed to write lba");
        }
    }

    fclose(fp);
    return 0;
}

static void close_sbi(struct erofs_sb_info *sbi)
{
    return;
}

static void close_tar(struct erofs_tarfile *erofstar)
{
    free(erofstar->ios.buffer);
}

int TarErofsInter::TarErofsImpl::extract_all() {
    struct erofs_sb_info sbi = {};
    struct erofs_tarfile erofstar = {};
    struct erofs_mkfs_cfg cfg;
    int err;

    /* initialization of sbi */
    err = init_sbi(&sbi, fout, reinterpret_cast<struct erofs_vfops*>(&target_vfops));
    if (err) {
        close_sbi(&sbi);
        LOG_ERROR("Failed to init sbi.");
        return err;
    }
    /* initialization of erofstar */
    err = init_tar(&erofstar, file, reinterpret_cast<struct erofs_vfops*>(&source_vfops));
    if (err) {
        close_sbi(&sbi);
        close_tar(&erofstar);
        LOG_ERROR("Failed to init tarerofs");
        return err;
    }

    cfg.sbi = &sbi;
    cfg.erofstar = &erofstar;
    cfg.incremental = !first_layer;
    cfg.ovlfs_strip = true;

    err = erofs_mkfs(&cfg);
    if (err) {
        close_sbi(&sbi);
        close_tar(&erofstar);
        LOG_ERROR("Failed to mkfs.");
        return err;
    }

    /* write mapfile */
    err = write_map_file(fout);
    if (err) {
        close_sbi(&sbi);
        close_tar(&erofstar);
        LOG_ERROR("Failed to write mapfile.");
        return err;
    }

    close_sbi(&sbi);
    close_tar(&erofstar);
    return 0;
}

TarErofsInter::TarErofsInter(photon::fs::IFile *file, photon::fs::IFile *target, uint64_t fs_blocksize,
          photon::fs::IFile *bf, bool meta_only, bool first_layer) :
          impl(new TarErofsImpl(file, target, fs_blocksize, bf, meta_only, first_layer))
{
}

TarErofsInter:: ~TarErofsInter()
{
    delete impl;
}

int TarErofsInter::extract_all()
{
    return impl->extract_all();
}
