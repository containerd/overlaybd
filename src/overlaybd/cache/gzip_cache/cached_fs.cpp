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
#include "cached_fs.h"
#include "../full_file_cache/cache_pool.h"
#include "../cache.h"
#include <photon/common/estring.h>

namespace Cache {

class GzipCachedFsImpl : public GzipCachedFs {
public:
    GzipCachedFsImpl(FileSystem::ICachePool *pool, size_t page_size,
                     size_t refill_unit, IOAlloc *io_alloc)
                 : pool_(pool), page_size_(page_size),
                 refill_unit_(refill_unit), io_alloc_(io_alloc) {
    }
    ~GzipCachedFsImpl() {
        delete pool_;
    }

    photon::fs::IFile *open_cached_gzip_file(photon::fs::IFile *file, const char *file_name) {
        if (!file) {
            LOG_ERRNO_RETURN(0, nullptr, "Open source gzfile failed");
        }
        estring fn = file_name;
        if (fn[0] != '/') {
            fn = estring().appends("/", fn);
        }
        auto cache_store = pool_->open(fn, O_RDWR | O_CREAT, 0644);
        if (cache_store == nullptr) {
            delete file;
            LOG_ERRNO_RETURN(0, nullptr, "file cache pool open file failed, name : `", file_name);
        }
        cache_store->set_src_file(file);
        cache_store->set_allocator(io_alloc_);
        cache_store->set_page_size(page_size_);
        auto ret = FileSystem::new_cached_file(cache_store, page_size_, nullptr);
        if (ret == nullptr) { // if create file is failed
            // file and cache_store must be release, or will leak
            delete file;
            cache_store->release();
        }
        return ret;
    }
private:
    FileSystem::ICachePool *pool_;
    size_t page_size_;
    size_t refill_unit_;
    IOAlloc *io_alloc_;
};

GzipCachedFs *new_gzip_cached_fs(photon::fs::IFileSystem *mediaFs, uint64_t refillUnit,
                                            uint64_t capacityInGB, uint64_t periodInUs,
                                            uint64_t diskAvailInBytes, IOAlloc *allocator) {
    if (refillUnit % 4096 != 0) {
        LOG_ERROR_RETURN(EINVAL, nullptr, "refill Unit need to be aligned to 4KB")
    }
    if (!allocator) {
        allocator = new IOAlloc;
    }
    FileCachePool *pool = nullptr;
    pool = new FileCachePool(mediaFs, capacityInGB, periodInUs, diskAvailInBytes, refillUnit);
    pool->Init();
    return new GzipCachedFsImpl(pool, 4096, refillUnit, allocator);
}
} // namespace Cache
