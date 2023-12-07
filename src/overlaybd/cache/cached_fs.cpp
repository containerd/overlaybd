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

#include "cache.h"
#include "photon/common/alog.h"
#include "photon/common/io-alloc.h"
#include "photon/common/iovector.h"
#include "photon/common/string_view.h"
#include "photon/fs/filesystem.h"
#include "photon/fs/range-split.h"
#include "pool_store.h"

namespace Cache {

using namespace FileSystem;
using namespace photon::fs;

const uint64_t kMaxPrefetchSize = 32 * 1024 * 1024;

class CachedFs : public ICachedFileSystem, public IFileSystemXAttr {
public:
    CachedFs(IFileSystem *srcFs, ICachePool *fileCachePool, size_t pageSize, IOAlloc *allocator,
             CacheFnTransFunc fn_trans_func)
        : srcFs_(srcFs), fileCachePool_(fileCachePool), pageSize_(pageSize), allocator_(allocator),
          xattrFs_(dynamic_cast<IFileSystemXAttr *>(srcFs)) {
        fileCachePool_->set_trans_func(fn_trans_func);
    }

    ~CachedFs() {
        delete fileCachePool_;
    }

    IFile *open(const char *pathname, int flags, mode_t mode) override {
        int cflags = (flags & (O_WRITE_THROUGH | O_CACHE_ONLY | O_WRITE_BACK));
        auto cache_store = fileCachePool_->open(pathname, O_RDWR | O_CREAT | cflags, 0644);
        if (nullptr == cache_store) {
            LOG_ERRNO_RETURN(0, nullptr, "fileCachePool_ open file failed, name : `", pathname)
        }

        cache_store->set_src_fs(srcFs_);
        cache_store->set_page_size(pageSize_);
        cache_store->set_allocator(allocator_);
        auto ret = new_cached_file(cache_store, pageSize_, this);
        if (ret == nullptr) { // if create file is failed
            // cache_store must be release, or will leak
            cache_store->release();
        }
        return ret;
    }

    IFile *open(const char *pathname, int flags) override {
        return open(pathname, flags, 0); // mode and flags are meaningless in RoCacheFS::open(2)(3)
    }

    int mkdir(const char *pathname, mode_t mode) override {
        return srcFs_ ? srcFs_->mkdir(pathname, mode) : -1;
    }

    int rmdir(const char *pathname) override {
        return srcFs_ ? srcFs_->rmdir(pathname) : -1;
    }

    ssize_t readlink(const char *path, char *buf, size_t bufsiz) override {
        return srcFs_ ? srcFs_->readlink(path, buf, bufsiz) : -1;
    }

    int rename(const char *oldname, const char *newname) override {
        return fileCachePool_->rename(oldname, newname);
    }

    int unlink(const char *filename) override {
        auto cache_store = fileCachePool_->open(filename, O_RDONLY, 0);
        if (cache_store != nullptr) {
            cache_store->set_cached_size(0);
            cache_store->set_actual_size(0);
            cache_store->release();
        }
        auto ret = fileCachePool_->evict(filename);
        return srcFs_ ? srcFs_->unlink(filename) : ret;
    }

    int statfs(const char *path, struct statfs *buf) override {
        return srcFs_ ? srcFs_->statfs(path, buf) : -1;
    }
    int statvfs(const char *path, struct statvfs *buf) override {
        return srcFs_ ? srcFs_->statvfs(path, buf) : -1;
    }
    int stat(const char *path, struct stat *buf) override {
        return srcFs_ ? srcFs_->stat(path, buf) : -1;
    }
    int lstat(const char *path, struct stat *buf) override {
        return srcFs_ ? srcFs_->lstat(path, buf) : -1;
    }

    int access(const char *pathname, int mode) override {
        if (srcFs_)
            return srcFs_->access(pathname, mode);
        auto cache_store = fileCachePool_->open(pathname, O_RDONLY, 0);
        if (cache_store == nullptr)
            return -1;
        cache_store->release();
        return 0;
    }

    DIR *opendir(const char *name) override {
        return srcFs_ ? srcFs_->opendir(name) : nullptr;
    }

    IFileSystem *get_source() override {
        return srcFs_;
    }

    int set_source(IFileSystem *src) override {
        srcFs_ = src;
        return 0;
    }

    ICachePool *get_pool() override {
        return fileCachePool_;
    }

    int set_pool(ICachePool *pool) override {
        fileCachePool_ = pool;
        return 0;
    }

    ssize_t getxattr(const char *path, const char *name, void *value, size_t size) override {
        return xattrFs_ ? xattrFs_->getxattr(path, name, value, size) : -1;
    }

    virtual ssize_t lgetxattr(const char *path, const char *name, void *value,
                              size_t size) override {
        return xattrFs_ ? xattrFs_->lgetxattr(path, name, value, size) : -1;
    }

    ssize_t listxattr(const char *path, char *list, size_t size) override {
        return xattrFs_ ? xattrFs_->listxattr(path, list, size) : -1;
    }

    ssize_t llistxattr(const char *path, char *list, size_t size) override {
        return xattrFs_ ? xattrFs_->llistxattr(path, list, size) : -1;
    }

    int setxattr(const char *path, const char *name, const void *value, size_t size,
                 int flags) override {
        return xattrFs_ ? xattrFs_->setxattr(path, name, value, size, flags) : -1;
    }

    int lsetxattr(const char *path, const char *name, const void *value, size_t size,
                  int flags) override {
        return xattrFs_ ? xattrFs_->lsetxattr(path, name, value, size, flags) : -1;
    }

    int removexattr(const char *path, const char *name) override {
        return xattrFs_ ? xattrFs_->removexattr(path, name) : -1;
    }

    int lremovexattr(const char *path, const char *name) override {
        return xattrFs_ ? xattrFs_->lremovexattr(path, name) : -1;
    }

    UNIMPLEMENTED_POINTER(IFile *creat(const char *pathname, mode_t mode));
    UNIMPLEMENTED(int symlink(const char *oldname, const char *newname));
    UNIMPLEMENTED(int link(const char *oldname, const char *newname));
    UNIMPLEMENTED(int chmod(const char *pathname, mode_t mode));
    UNIMPLEMENTED(int chown(const char *pathname, uid_t owner, gid_t group));
    UNIMPLEMENTED(int lchown(const char *pathname, uid_t owner, gid_t group));
    UNIMPLEMENTED(int truncate(const char *path, off_t length));
    UNIMPLEMENTED(int utime(const char *path, const struct utimbuf *file_times));
    UNIMPLEMENTED(int utimes(const char *path, const struct timeval times[2]));
    UNIMPLEMENTED(int lutimes(const char *path, const struct timeval times[2]));
    UNIMPLEMENTED(int mknod(const char *path, mode_t mode, dev_t dev));
    UNIMPLEMENTED(int syncfs());

private:
    IFileSystem *srcFs_;        // owned by extern
    ICachePool *fileCachePool_; //  owned by current class
    size_t pageSize_;

    IOAlloc *allocator_;
    IFileSystemXAttr *xattrFs_;
};

/*
 *  the procedures of pread are as follows:
 *  1. check that the cache is hit(contain unaligned block).
 *  2. if hit, just read from cache.
 *  3. if not, merge all holes into one read request(offset, size),
 *     then read missing data from source of file and write it into cache,
 *     after that read cache' data into user's buffer.
 */
class CachedFile : public ICachedFile, public IFileXAttr {
public:
    CachedFile(ICacheStore *cache_store, size_t pageSize, IFileSystem *fs)
        : cache_store_(cache_store), pageSize_(pageSize), fs_(fs) {
    }

    ~CachedFile() {
        cache_store_->release();
    }

    IFileSystem *filesystem() override {
        return fs_;
    }

    ssize_t pread(void *buf, size_t count, off_t offset) override {
        struct iovec v {
            buf, count
        };
        return preadv(&v, 1, offset);
    }

    ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override {
        return preadv2(iov, iovcnt, offset, 0);
    }

    ssize_t preadv2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override {
        return cache_store_->preadv2(iov, iovcnt, offset, flags);
    }

    //  pwrite* need to be aligned to 4KB for avoiding write padding.
    ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        struct iovec v {
            const_cast<void *>(buf), count
        };
        return pwritev(&v, 1, offset);
    }

    ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override {
        return pwritev2(iov, iovcnt, offset, 0);
    }

    ssize_t pwritev2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override {
        return cache_store_->pwritev2(iov, iovcnt, offset, flags);
    }

    int fstat(struct stat *buf) override {
        DEFER({ buf->st_ino = cache_store_->get_handle(); });
        auto size = cache_store_->get_actual_size();
        if (size % pageSize_ != 0) {
            buf->st_size = size;
            return 0;
        }
        IFile *src_file = nullptr;
        if (cache_store_->open_src_file(&src_file) != 0)
            return -1;
        if (src_file)
            return src_file->fstat(buf);
        return cache_store_->fstat(buf);
    }

    int close() override {
        return 0;
    }

    ssize_t read(void *buf, size_t count) override {
        struct iovec v {
            buf, count
        };
        return readv(&v, 1);
    }

    ssize_t readv(const struct iovec *iov, int iovcnt) override {
        auto ret = preadv(iov, iovcnt, readOffset_);
        if (ret > 0) {
            readOffset_ += ret;
        }
        return ret;
    }

    ssize_t write(const void *buf, size_t count) override {
        struct iovec v {
            const_cast<void *>(buf), count
        };
        return writev(&v, 1);
    }

    ssize_t writev(const struct iovec *iov, int iovcnt) override {
        auto ret = pwritev(iov, iovcnt, writeOffset_);
        if (ret > 0) {
            writeOffset_ += ret;
        }
        return ret;
    }

    int query(off_t offset, size_t count) override {
        auto ret = cache_store_->queryRefillRange(offset, count);
        if (ret.first < 0)
            return -1;
        return ret.second;
    }

    //  offset and len must be aligned 4k, otherwise it's useless.
    //  !!! need ensure no other read operation, otherwise read may read hole data(zero).
    int fallocate(int mode, off_t offset, off_t len) override {
        if (len == -1) {
            return cache_store_->evict(offset, len);
        }
        range_split rs(offset, len, pageSize_);
        auto aligned_offset = rs.aligned_begin_offset();
        auto aligned_len = rs.aligned_length();
        LOG_DEBUG(VALUE(offset), VALUE(len), VALUE(aligned_offset), VALUE(aligned_len));
        return cache_store_->evict(aligned_offset, aligned_len);
    }

    int fadvise(off_t offset, off_t len, int advice) override {
        if (advice == POSIX_FADV_WILLNEED) {
            int ret = prefetch(len, offset, 0);
            if (ret < 0) {
                LOG_ERROR_RETURN(0, -1, "prefetch read failed");
            }
            return 0;
        }
        LOG_ERRNO_RETURN(ENOSYS, -1, "advice ` is not implemented", advice);
    }

    IFile *get_source() override {
        IFile *src = nullptr;
        if (cache_store_->open_src_file(&src) != 0)
            return nullptr;
        return src;
    }

    inline void get_source_filexattr() {
        if (!source_filexattr_) {
            auto sfile = get_source();
            source_filexattr_ = dynamic_cast<IFileXAttr *>(sfile);
        }
    }

    // set the source file system, and enable `auto_refill`
    int set_source(IFile *src) override {
        cache_store_->set_src_file(src);
        return 0;
    }

    ICacheStore *get_store() override {
        return cache_store_;
    }

    int ftruncate(off_t length) override {
        cache_store_->set_cached_size(length);
        cache_store_->set_actual_size(length);
        return 0;
    }

    std::string_view get_pathname() {
        return get_store()->get_src_name();
    }

    ssize_t fgetxattr(const char *name, void *value, size_t size) override {
        get_source_filexattr();
        return source_filexattr_ ? source_filexattr_->fgetxattr(name, value, size) : -1;
    }

    ssize_t flistxattr(char *list, size_t size) override {
        get_source_filexattr();
        return source_filexattr_ ? source_filexattr_->flistxattr(list, size) : -1;
    }

    int fsetxattr(const char *name, const void *value, size_t size, int flags) override {
        get_source_filexattr();
        return source_filexattr_ ? source_filexattr_->fsetxattr(name, value, size, flags) : -1;
    }

    int fremovexattr(const char *name) override {
        get_source_filexattr();
        return source_filexattr_ ? source_filexattr_->fremovexattr(name) : -1;
    }

    UNIMPLEMENTED(off_t lseek(off_t offset, int whence));
    UNIMPLEMENTED(int fsync());
    UNIMPLEMENTED(int fdatasync());
    UNIMPLEMENTED(int fchmod(mode_t mode));
    UNIMPLEMENTED(int fchown(uid_t owner, gid_t group));
    UNIMPLEMENTED(int fiemap(photon::fs::fiemap *map));

protected:
    ssize_t prefetch(size_t count, off_t offset, int flags) {
        uint64_t end = photon::sat_add(offset, count);
        if (offset % pageSize_ != 0) {
            offset = offset / pageSize_ * pageSize_;
        }
        if (end % pageSize_ != 0) {
            end = photon::sat_add(end, pageSize_ - 1) / pageSize_ * pageSize_;
        }

        uint64_t remain = end - offset;
        ssize_t read = 0;
        while (remain > 0) {
            off_t min = std::min(kMaxPrefetchSize, remain);
            remain -= min;
            auto ret = cache_store_->try_refill_range(offset, static_cast<size_t>(min));
            if (ret < 0) {
                LOG_ERRNO_RETURN(0, -1,
                                 "try_refill_range failed, ret : `, len : `, offset : `, flags : `",
                                 ret, min, offset, flags);
            }
            read += ret;
            //  read end of file.
            if (ret < min) {
                return read;
            }
            offset += ret;
        }
        return read;
    }

    ICacheStore *cache_store_;
    size_t pageSize_;
    IFileSystem *fs_;

    off_t readOffset_ = 0;
    off_t writeOffset_ = 0;
    IFileXAttr *source_filexattr_ = nullptr;
};

} //  namespace Cache

namespace FileSystem {
using namespace photon::fs;
ICachedFileSystem *new_cached_fs(IFileSystem *src, ICachePool *pool, uint64_t pageSize,
                                 IOAlloc *allocator, CacheFnTransFunc fn_trans_func) {
    if (!allocator) {
        allocator = new IOAlloc;
    }
    return new ::Cache::CachedFs(src, pool, pageSize, allocator, fn_trans_func);
}

ICachedFile *new_cached_file(ICacheStore *store, uint64_t pageSize, IFileSystem *fs) {
    return new ::Cache::CachedFile(store, pageSize, fs);
}
} //  namespace FileSystem
