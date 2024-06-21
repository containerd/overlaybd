#include "erofs/tar.h"
#include "erofs/io.h"
#include <photon/fs/filesystem.h>

#define SECTOR_SIZE 512ULL
#define SECTOR_BITS 9

struct erofs_vfops_wrapper {
	struct erofs_vfops ops;
	void *private_data;
};

struct erofs_sector {
   int64_t addr;
   bool dirty;
   char data[SECTOR_SIZE];
};

class ErofsCache {
public:
    ErofsCache(photon::fs::IFile *f, int ord) {
        file = f;
        order = ord;
        caches = (struct erofs_sector*)malloc((1 << order) * sizeof(struct erofs_sector));
        for (int i = 0; i < (1 << order); i ++) {
            ((struct erofs_sector *)caches + i)->addr = -1;
            ((struct erofs_sector *)caches + i)->dirty = false;
        }
    }

    ~ErofsCache() {
        free(caches);
    }

   ssize_t write_sector(u64 addr, char *buf);
   ssize_t read_sector(u64 addr, char *buf);
   int flush();

public:
    struct erofs_sector *caches;
    photon::fs::IFile *file;
    int order;
};

class TarErofsInter::TarErofsImpl {
public:
    TarErofsImpl(photon::fs::IFile *file, photon::fs::IFile *target, uint64_t fs_blocksize = 4096,
          photon::fs::IFile *bf = nullptr, bool meta_only = true, bool first_layer = true)
        : file(file), fout(target), fs_base_file(bf), meta_only(meta_only), first_layer(first_layer),
        erofs_cache(target, 7){

        target_vfops.ops.pread = target_pread;
        target_vfops.ops.pwrite = target_pwrite;
        target_vfops.ops.fsync = target_fsync;
        target_vfops.ops.fallocate = target_fallocate;
        target_vfops.ops.ftruncate = target_ftruncate;
        target_vfops.ops.read = target_read;
        target_vfops.ops.lseek = target_lseek;
        target_vfops.private_data = (void*)this;

        source_vfops.ops.pread = source_pread;
        source_vfops.ops.pwrite = source_pwrite;
        source_vfops.ops.fsync = source_fsync;
        source_vfops.ops.fallocate = source_fallocate;
        source_vfops.ops.ftruncate = source_ftruncate;
        source_vfops.ops.read = source_read;
        source_vfops.ops.lseek = source_lseek;
        source_vfops.private_data = (void*)this;
    }

    int extract_all();

public:
    photon::fs::IFile *file = nullptr;     // source
    photon::fs::IFile *fout = nullptr; // target
    photon::fs::IFile *fs_base_file = nullptr;
    bool meta_only;
    bool first_layer;
    struct erofs_vfops_wrapper target_vfops;
    struct erofs_vfops_wrapper source_vfops;
    ErofsCache erofs_cache;
public:
    /* I/O control for target */
    static ssize_t target_pread(struct erofs_vfile *vf, void *buf, u64 offset, size_t len);
    static ssize_t target_pwrite(struct erofs_vfile *vf, const void *buf, u64 offset, size_t len);
    static int target_fsync(struct erofs_vfile *vf);
    static int target_fallocate(struct erofs_vfile *vf, u64 offset, size_t len, bool pad);
    static int target_ftruncate(struct erofs_vfile *vf, u64 length);
    static ssize_t target_read(struct erofs_vfile *vf, void *buf, size_t len);
    static off_t target_lseek(struct erofs_vfile *vf, u64 offset, int whence);

    /* I/O control for source */
    static ssize_t source_pread(struct erofs_vfile *vf, void *buf, u64 offset, size_t len);
    static ssize_t source_pwrite(struct erofs_vfile *vf, const void *buf, u64 offset, size_t len);
    static int source_fsync(struct erofs_vfile *vf);
    static int source_fallocate(struct erofs_vfile *vf, u64 offset, size_t len, bool pad);
    static int source_ftruncate(struct erofs_vfile *vf, u64 length);
    static ssize_t source_read(struct erofs_vfile *vf, void *buf, size_t len);
    static off_t source_lseek(struct erofs_vfile *vf, u64 offset, int whence);

    /* helper function */
    static TarErofsImpl *ops_to_tarerofsimpl(struct erofs_vfops *ops);
};
