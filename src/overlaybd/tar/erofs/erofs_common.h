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

// common contents used internally by erofs

#pragma once

#include "erofs/io.h"
#include <photon/fs/filesystem.h>
#include <map>
#include <set>

/* block-related definitions */
#define SECTOR_SIZE 512ULL
#define SECTOR_BITS 9

/* address alignment operation */
#define round_down_blk(addr) ((addr) & (~(SECTOR_SIZE - 1)))
#define round_up_blk(addr) (round_down_blk((addr) + SECTOR_SIZE - 1))
#define erofs_min(a, b) (a) < (b) ? (a) : (b)

#define EROFS_ROOT_XATTR_SZ (16 * 1024)

#define EROFS_UNIMPLEMENTED 1

#define EROFS_UNIMPLEMENTED_FUNC(ret_type, cls, func, ret) \
ret_type cls::func { \
	return ret; \
}

struct liberofs_inmem_sector {
	char data[SECTOR_SIZE];
};

/*
 * Internal cache of EROFS, used to accelerate
 * the read and write operations of an IFile.
 */
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

/*
 * Encapsulation of IFile by liberofs,
 * including I/O operations and ErofsCache.
 */
struct liberofs_file {
	struct erofs_vfops ops;
	photon::fs::IFile *file;
	ErofsCache *cache;
};

/* helper functions for reading and writing photon files */
extern ssize_t erofs_read_photon_file(void *buf, u64 offset, size_t len,
				      ErofsCache *cache);
extern ssize_t erofs_write_photon_file(const void *buf, u64 offset,
				       size_t len, ErofsCache *cache);

/* I/O controllers for target */
extern ssize_t erofs_target_pread(struct erofs_vfile *vf, void *buf,
				  u64 offset, size_t len);
extern ssize_t erofs_target_pwrite(struct erofs_vfile *vf, const void *buf,
				   u64 offset, size_t len);
extern int erofs_target_fsync(struct erofs_vfile *vf);
extern int erofs_target_fallocate(struct erofs_vfile *vf, u64 offset,
				  size_t len, bool pad);
extern int erofs_target_ftruncate(struct erofs_vfile *vf, u64 length);
extern ssize_t erofs_target_read(struct erofs_vfile *vf, void *buf, size_t len);
extern off_t erofs_target_lseek(struct erofs_vfile *vf, u64 offset, int whence);

/* I/O controllers for source */
extern ssize_t erofs_source_pread(struct erofs_vfile *vf, void *buf, u64 offset,
				  size_t len);
extern ssize_t erofs_source_pwrite(struct erofs_vfile *vf, const void *buf,
				   u64 offset, size_t len);
extern int erofs_source_fsync(struct erofs_vfile *vf);
extern int erofs_source_fallocate(struct erofs_vfile *vf,
				  u64 offset, size_t len, bool pad);
extern int erofs_source_ftruncate(struct erofs_vfile *vf, u64 length);
extern ssize_t erofs_source_read(struct erofs_vfile *vf, void *buf,
				 size_t bytes);
extern off_t erofs_source_lseek(struct erofs_vfile *vf, u64 offset,
				int whence);
