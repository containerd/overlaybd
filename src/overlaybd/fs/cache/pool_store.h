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
#include <assert.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../../string_view.h"
#include "../../string-keyed.h"
#include "../../object.h"

struct iovector;

namespace FileSystem {
class ICacheStore;
struct CacheStat {
    uint32_t struct_size = sizeof(CacheStat);
    uint32_t refill_unit; // in bytes
    uint32_t total_size;  // in refill_unit
    uint32_t used_size;   // in refill_unit
};

class ICachePool : public Object {
public:
    virtual ICacheStore *open(std::string_view filename, int flags, mode_t mode);

    // if pathname is {nullptr, 0} or "/", returns the overall stat
    virtual int stat(CacheStat *stat, std::string_view pathname = std::string_view(nullptr, 0)) = 0;

    // force to evict specified files(s)
    virtual int evict(std::string_view filename) = 0;

    // try to evict at least `size` bytes, and also make sure
    // available space meet other requirements as well
    virtual int evict(size_t size = 0) = 0;

    int store_release(ICacheStore *store);

    virtual ICacheStore *do_open(std::string_view filename, int flags, mode_t mode) = 0;

    ICacheStore *find_store_map(std::string_view pathname);

protected:
    unordered_map_string_key<ICacheStore *> m_stores;
};

class ICacheStore : public Object {
public:
    struct try_preadv_result {
        size_t iov_sum;     // sum of the iovec[]
        size_t refill_size; // size in bytes to refill, 0 means cache hit
        union {
            off_t refill_offset; // the offset to fill, if not hit
            ssize_t size;        // the return value of preadv(), if hit
        };
    };

    // either override try_preadv() or try_preadv_mutable()
    virtual try_preadv_result try_preadv(const struct iovec *iov, int iovcnt, off_t offset);
    virtual try_preadv_result try_preadv_mutable(struct iovec *iov, int iovcnt, off_t offset);

    // either override preadv() or preadv_mutable()
    virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset);
    virtual ssize_t preadv_mutable(struct iovec *iov, int iovcnt, off_t offset);

    // either override pwritev() or pwritev_mutable()
    virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset);
    virtual ssize_t pwritev_mutable(struct iovec *iov, int iovcnt, off_t offset);

    virtual int stat(CacheStat *stat) = 0;
    virtual int evict(off_t offset, size_t count = -1) = 0;

    void release() {
        ref_count--;
        if (ref_count == 0) {
            pool_->store_release(this);
            try_destruct();
        }
    }
    void add_ref() {
        ref_count++;
    }

    uint32_t get_ref_count() {
        return ref_count;
    }

    bool try_destruct() {
        if (ref_count == 0) {
            delete this;
            return true;
        }
        return false;
    }

    ssize_t pread(void *buf, size_t count, off_t offset) {
        struct iovec iov {
            buf, count
        };
        return preadv_mutable(&iov, 1, offset);
    }
    ssize_t pwrite(const void *buf, size_t count, off_t offset) {
        struct iovec iov {
            (void *)buf, count
        };
        return pwritev_mutable(&iov, 1, offset);
    }

    // offset + size must <= origin file size
    virtual std::pair<off_t, size_t> queryRefillRange(off_t offset, size_t size) = 0;

    virtual int fstat(struct stat *buf) = 0;

    virtual std::string_view get_pathname() {
        return f_name_;
    };

    virtual void set_pathname(std::string_view pathname) {
        f_name_ = pathname;
    }

    virtual uint32_t get_refcount() {
        return ref_count;
    }

    virtual void set_pool(ICachePool *pool) {
        pool_ = pool;
    }

protected:
    uint32_t ref_count = 0; // store's referring count
    std::string_view f_name_;
    ICachePool *pool_;
    ~ICacheStore(){};
};

class IMemCacheStore : public ICacheStore {
public:
    // Get the internal buffer for the specified LBA range (usually aligned),
    // which will remain valid for user until released by unpin_buffer().
    // Will allocate pages for missed ranges.
    // Will refill / fetch / load data from source if `refill`.
    // Concurrent R/W to a same range are guaranteed to work, but considered
    // a race-condition and the result is undefiend.
    // returns # of bytes actually got, or <0 for failures
    virtual ssize_t pin_buffer(off_t offset, size_t count, /*OUT*/ iovector *iov) = 0;

    // Release buffers got from pin_buffer(),
    // and the buffer is no longer valid for user.
    // return 0 for success, < 0 for failures
    virtual int unpin_buffer(off_t offset, size_t count) = 0;
};
} // namespace FileSystem
