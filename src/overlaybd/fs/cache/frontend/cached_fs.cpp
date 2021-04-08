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

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "../cache.h"
#include "cached_file.h"
#include "../../../alog.h"
#include "../../../io-alloc.h"

namespace Cache {

using namespace FileSystem;

class CachedFs : public ICachedFileSystem {
public:
    CachedFs(IFileSystem *srcFs, ICachePool *fileCachePool, size_t pageSize, size_t refillUnit,
             IOAlloc *allocator)
        : srcFs_(srcFs), fileCachePool_(fileCachePool), pageSize_(pageSize),
          refillUnit_(refillUnit), allocator_(allocator) {
    }

    ~CachedFs() {
        delete fileCachePool_;
    }

    IFile *open(const char *pathname, int flags, mode_t mode) {
        IFile *srcFile = nullptr;
        if (srcFs_) {
            srcFile = srcFs_->open(pathname, O_RDONLY);
            if (!srcFile)
                LOG_ERRNO_RETURN(0, nullptr, "Open source file failed");
        }

        auto cache_store = fileCachePool_->open(pathname, O_RDWR | O_CREAT, 0644);
        if (nullptr == cache_store) {
            delete srcFile;
            LOG_ERRNO_RETURN(0, nullptr, "fileCachePool_ open file failed, name : `", pathname)
        }

        auto ret = new_cached_file(srcFile, cache_store, pageSize_, refillUnit_, allocator_, this);
        if (ret == nullptr) { // if create file is failed
            // srcFile and cache_store must be release, or will leak
            delete srcFile;
            cache_store->release();
        }
        return ret;
    }

    IFile *open(const char *pathname, int flags) {
        return open(pathname, flags, 0); // mode and flags are meaningless in RoCacheFS::open(2)(3)
    }

    UNIMPLEMENTED_POINTER(IFile *creat(const char *pathname, mode_t mode));
    UNIMPLEMENTED(int mkdir(const char *pathname, mode_t mode));
    UNIMPLEMENTED(int rmdir(const char *pathname));
    UNIMPLEMENTED(int symlink(const char *oldname, const char *newname));

    ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
        return srcFs_ ? srcFs_->readlink(path, buf, bufsiz) : -1;
    }

    UNIMPLEMENTED(int link(const char *oldname, const char *newname));
    UNIMPLEMENTED(int rename(const char *oldname, const char *newname));
    UNIMPLEMENTED(int unlink(const char *filename));
    UNIMPLEMENTED(int chmod(const char *pathname, mode_t mode));
    UNIMPLEMENTED(int chown(const char *pathname, uid_t owner, gid_t group));
    UNIMPLEMENTED(int lchown(const char *pathname, uid_t owner, gid_t group));

    int statfs(const char *path, struct statfs *buf) {
        return srcFs_ ? srcFs_->statfs(path, buf) : -1;
    }
    int statvfs(const char *path, struct statvfs *buf) {
        return srcFs_ ? srcFs_->statvfs(path, buf) : -1;
    }
    int stat(const char *path, struct stat *buf) {
        return srcFs_ ? srcFs_->stat(path, buf) : -1;
    }
    int lstat(const char *path, struct stat *buf) {
        return srcFs_ ? srcFs_->stat(path, buf) : -1;
    }
    int access(const char *pathname, int mode) {
        return srcFs_ ? srcFs_->access(pathname, mode) : -1;
    }

    UNIMPLEMENTED(int truncate(const char *path, off_t length));
    UNIMPLEMENTED(int syncfs());

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

private:
    IFileSystem *srcFs_;        // owned by extern
    ICachePool *fileCachePool_; //  owned by current class
    size_t pageSize_;
    size_t refillUnit_;

    IOAlloc *allocator_;
};

} //  namespace Cache

namespace FileSystem {
ICachedFileSystem *new_cached_fs(IFileSystem *src, ICachePool *pool, uint64_t pageSize,
                                 uint64_t refillUnit, IOAlloc *allocator) {
    if (!allocator) {
        allocator = new IOAlloc;
    }
    return new ::Cache::CachedFs(src, pool, pageSize, refillUnit, allocator);
}
} // namespace FileSystem
