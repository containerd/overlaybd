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
#pragma once

#include <stddef.h>
#include <sys/uio.h>
#include <memory>
#include <vector>
#include <photon/fs/filesystem.h>
#include "../cache.h"
#include <photon/common/range-lock.h>
#include <photon/common/string_view.h>

struct IOAlloc;

namespace FileSystem {
class ICacheStore;
}

namespace Cache {
using namespace FileSystem;

/*
 *  the procedures of pread are as follows:
 *  1. check that the cache is hit(contain unaligned block).
 *  2. if hit, just read from cache.
 *  3. if not, merge all holes into one read request(offset, size),
 *     then read missing data from source of file and write it into cache,
 *     after that read cache' data into user's buffer.
 */

class CachedFile : public ICachedFile {
public:
    CachedFile(IFile *src_file, ICacheStore *cache_store, off_t size, uint64_t pageSize,
               uint64_t refillUnit, IOAlloc *allocator, photon::fs::IFileSystem *fs);
    ~CachedFile();

    photon::fs::IFileSystem *filesystem();

    ssize_t pread(void *buf, size_t count, off_t offset) override;
    ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override;

    //  pwrite* need to be aligned to 4KB for avoiding write padding.
    ssize_t pwrite(const void *buf, size_t count, off_t offset) override;
    ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override;

    UNIMPLEMENTED(off_t lseek(off_t offset, int whence));
    UNIMPLEMENTED(int fsync());
    UNIMPLEMENTED(int fdatasync());
    UNIMPLEMENTED(int fchmod(mode_t mode));
    UNIMPLEMENTED(int fchown(uid_t owner, gid_t group));
    int fstat(struct stat *buf) override;

    int close();

    ssize_t read(void *buf, size_t count) override;
    ssize_t readv(const struct iovec *iov, int iovcnt) override;
    ssize_t write(const void *buf, size_t count) override;
    ssize_t writev(const struct iovec *iov, int iovcnt) override;

    int fiemap(struct photon::fs::fiemap *map) override;

    int query(off_t offset, size_t count) override;

    //  offset and len must be aligned 4k, otherwise it's useless.
    //  !!! need ensure no other read operation, otherwise read may read hole data(zero).
    int fallocate(int mode, off_t offset, off_t len) override;

    IFile *get_source() override {
        return src_file_;
    }

    // set the source file system, and enable `auto_refill`
    int set_source(IFile *src) override {
        src_file_ = src;
        return 0;
    }

    ICacheStore *get_store() override {
        return cache_store_;
    }

    int ftruncate(off_t length) override {
        assert(!src_file_);
        size_ = length;
        return 0;
    }

    std::string_view get_pathname();

private:
    ssize_t prefetch(size_t count, off_t offset);

    ssize_t preadvInternal(const struct iovec *iov, int iovcnt, off_t offset);

    IFile *src_file_;          //  owned by current class
    ICacheStore *cache_store_; //  owned by current class
    off_t size_;
    size_t pageSize_;
    size_t refillUnit_;

    RangeLock rangeLock_;

    IOAlloc *allocator_;
    photon::fs::IFileSystem *fs_;

    off_t readOffset_;
    off_t writeOffset_;
};

ICachedFile *new_cached_file(photon::fs::IFile *src, ICacheStore *store, uint64_t pageSize,
                             uint64_t refillUnit, IOAlloc *allocator, photon::fs::IFileSystem *fs);

} //  namespace Cache
