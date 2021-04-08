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
#include "cached_file.h"
#include <sys/stat.h>
#include <sys/uio.h>
#include "../../../alog-audit.h"
#include "../../../alog-stdstring.h"
#include "../../../alog.h"
#include "../../../iovector.h"
#include "../../../utility.h"
#include "../pool_store.h"

#ifdef CACHE_BENCHMARK
#include "test/metric.h"

inline uint64_t GetSteadyTimeNs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}
#endif

namespace Cache {

const off_t kMaxPrefetchSize = 16 * 1024 * 1024;

CachedFile::CachedFile(FileSystem::IFile *src_file, FileSystem::ICacheStore *cache_store,
                       off_t size, size_t pageSize, size_t refillUnit, IOAlloc *allocator,
                       IFileSystem *fs)
    : src_file_(src_file), cache_store_(cache_store), size_(size), pageSize_(pageSize),
      refillUnit_(refillUnit), allocator_(allocator), fs_(fs), readOffset_(0), writeOffset_(0){};

CachedFile::~CachedFile() {
    cache_store_->release();
    delete src_file_;
}

IFileSystem *CachedFile::filesystem() {
    return fs_;
}

ssize_t CachedFile::pread(void *buf, size_t count, off_t offset) {
    struct iovec v {
        buf, count
    };
    return preadv(&v, 1, offset);
}

ssize_t CachedFile::preadv(const struct iovec *iov, int iovcnt, off_t offset) {
    if (1 == iovcnt && !iov->iov_base) {
        return prefetch(iov->iov_len, offset);
    }
    return preadvInternal(iov, iovcnt, offset);
}

ssize_t CachedFile::prefetch(size_t count, off_t offset) {
    static char buf[kMaxPrefetchSize * 2] = {0};
    void *alignBuf = align_ptr(buf, pageSize_);
    struct iovec iov {
        alignBuf, count
    };

    auto end = offset + count;
    if (offset % pageSize_ != 0) {
        offset = offset & ~(pageSize_ - 1);
    }
    if (end % pageSize_ != 0) {
        end = (end + pageSize_ - 1) & ~(pageSize_ - 1);
    }

    off_t remain = end - offset;
    ssize_t read = 0;
    while (remain > 0) {
        off_t min = std::min(kMaxPrefetchSize, remain);
        remain -= min;
        iov.iov_len = min;
        auto ret = preadvInternal(&iov, 1, offset);
        if (ret < 0) {
            LOG_ERRNO_RETURN(0, -1, "preadv failed, ret : `, len : `, offset : `, size_ : `", ret,
                             min, offset, size_);
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

ssize_t CachedFile::preadvInternal(const struct iovec *iov, int iovcnt, off_t offset) {
#ifdef CACHE_BENCHMARK
    auto begin = GetSteadyTimeNs();
#endif
    if (offset < 0) {
        LOG_ERROR_RETURN(EINVAL, -1, "offset is invalid, offset : `", offset)
    }

    iovector_view view(const_cast<struct iovec *>(iov), iovcnt);
    size_t iovSize = view.sum();
    if (0u == iovSize) {
        return 0;
    }

    if (offset >= size_ || offset + static_cast<off_t>(iovSize) > size_) {
        struct stat st;
        auto ok = fstat(&st);
        if (ok == 0 && st.st_size > size_) {
            off_t last = alingn_down(size_, pageSize_);
            if (last != size_)
                cache_store_->evict(last, pageSize_);
            size_ = st.st_size;
        }
    }

    if (offset >= size_) {
        return 0;
    }

    IOVector input(iov, iovcnt);
    if (offset + static_cast<off_t>(iovSize) > size_) {
        input.extract_back(offset + static_cast<off_t>(iovSize) - size_);
        iovSize = size_ - offset;
    }

again:
    auto tr = cache_store_->try_preadv(input.iovec(), input.iovcnt(), offset);
    if (tr.refill_offset < 0) {
        if (src_file_) {
            ssize_t ret;
            SCOPE_AUDIT("download", AU_FILEOP(get_pathname(), offset, ret));
            ret = src_file_->preadv(input.iovec(), input.iovcnt(), offset);
            return ret;
        }

        return -1;
    } else if (tr.refill_size == 0 && tr.size >= 0) {
#ifdef CACHE_BENCHMARK
        gMetric->readHit_.Increase(1);
        gMetric->readLatency_.Increase((GetSteadyTimeNs() - begin) / 100.0);
#endif
        return tr.size;
    }

    if (!src_file_) {
        return -1;
    }

    uint64_t refillOff = tr.refill_offset;
    uint64_t refillSize = tr.refill_size;
    if (refillOff + refillSize > static_cast<uint64_t>(size_)) {
        refillSize = size_ - refillOff;
    }

    int ret = rangeLock_.try_lock_wait(refillOff, refillSize);
    if (ret < 0) {
        goto again;
    }

    IOVector buffer(*allocator_);
    {
        DEFER(rangeLock_.unlock(refillOff, refillSize));
        auto alloc = buffer.push_back(refillSize);
        if (alloc < refillSize) {
            LOG_ERROR("memory allocate failed, refillSize:`, alloc:`", refillSize, alloc);
            ssize_t ret;
            SCOPE_AUDIT("download", AU_FILEOP(get_pathname(), offset, ret));
            ret = src_file_->preadv(input.iovec(), input.iovcnt(), offset);
            return ret;
        }

        ssize_t read;
        {
            SCOPE_AUDIT("download", AU_FILEOP(get_pathname(), offset, read));
            read = src_file_->preadv(buffer.iovec(), buffer.iovcnt(), refillOff);
        }

        if (read != static_cast<ssize_t>(refillSize)) {
            LOG_ERRNO_RETURN(
                0, -1,
                "src file read failed, read : `, expectRead : `, size_ : `, offset : `, sum : `",
                read, refillSize, size_, refillOff, buffer.sum());
        }

        auto write = cache_store_->pwritev(buffer.iovec(), buffer.iovcnt(), refillOff);

        if (write != static_cast<ssize_t>(refillSize)) {
            if (ENOSPC != errno)
                LOG_ERROR("cache file write failed : `, error : `, size_ : `, offset : `, sum : `",
                          write, ERRNO(errno), size_, refillOff, buffer.sum());
            ssize_t ret;
            {
                SCOPE_AUDIT("download", AU_FILEOP(get_pathname(), offset, ret));
                ret = src_file_->preadv(input.iovec(), input.iovcnt(), offset);
            }
            return ret;
        }

#ifdef CACHE_BENCHMARK
        gMetric->writeQps_.Increase(1);
#endif
    }

    IOVector refillBuf(buffer.iovec(), buffer.iovcnt());
    int remain = iovSize;
    int result = 0;
    if (tr.refill_offset <= offset) {
        auto inView = input.view();
        refillBuf.extract_front(offset - tr.refill_offset);
        auto copy = refillBuf.memcpy_to(&inView, iovSize);
        remain -= copy;
        offset += copy;
        result += copy;
    } else if (tr.refill_offset + tr.refill_size >= offset + iovSize) {
        iovector_view tailIov;
        tailIov.iovcnt = 0;
        input.slice(iovSize - (tr.refill_offset - offset), tr.refill_offset - offset, &tailIov);
        auto copy = refillBuf.memcpy_to(&tailIov);
        input.extract_back(copy);
        result += copy;
        remain -= copy;
    }

    if (remain > 0) {
        auto readRet = cache_store_->preadv(input.iovec(), input.iovcnt(), offset);
        if (readRet < 0) {
            SCOPE_AUDIT("download", AU_FILEOP(get_pathname(), offset, readRet));
            readRet = src_file_->preadv(input.iovec(), input.iovcnt(), offset);
            if (readRet < 0)
                LOG_ERRNO_RETURN(0, readRet, "read failed, ret:`, offset:`,sum:`,size_:`", readRet,
                                 offset, input.sum(), size_);
        }

        return result + readRet;
    }

    return result;
}

ssize_t CachedFile::pwrite(const void *buf, size_t count, off_t offset) {
    struct iovec v {
        const_cast<void *>(buf), count
    };
    return pwritev(&v, 1, offset);
}

ssize_t CachedFile::pwritev(const struct iovec *iov, int iovcnt, off_t offset) {
    if (offset >= size_) {
        return 0;
    }

    iovector_view view(const_cast<struct iovec *>(iov), iovcnt);
    size_t size = view.sum();

    if (offset % pageSize_ != 0 ||
        (size % pageSize_ != 0 && offset + static_cast<off_t>(size) < size_)) {
        LOG_ERROR_RETURN(EINVAL, -1, "size or offset is not aligned to 4K, size : `, offset : `",
                         size, offset);
    }

    if (offset + static_cast<off_t>(size) <= size_) {
        return cache_store_->pwritev(iov, iovcnt, offset);
    }

    IOVector ioVector(iov, iovcnt);
    if (offset + static_cast<off_t>(size) > size_) {
        auto ret = ioVector.extract_back(size - (size_ - offset));
        if (ret != size - (size_ - offset))
            LOG_ERRNO_RETURN(EINVAL, -1, "extract failed, extractSize : `, expected : ", ret,
                             size - (size_ - offset))
    }

    auto write = cache_store_->pwritev(ioVector.iovec(), ioVector.iovcnt(), offset);
    if (write != static_cast<ssize_t>(ioVector.sum())) {
        if (ENOSPC != errno)
            LOG_ERROR("cache file write failed : `, error : `, size_ : `, offset : `, sum : `",
                      write, ERRNO(errno), size_, offset, ioVector.sum());
    }

    return write;
}

int CachedFile::fiemap(struct fiemap *map) {
    errno = ENOSYS;
    return -1;
}

int CachedFile::query(off_t offset, size_t count) {
    auto ret = cache_store_->queryRefillRange(offset, count);
    return ret.second;
}

int CachedFile::fallocate(int mode, off_t offset, off_t len) {
    if (offset % pageSize_ != 0 || len % pageSize_ != 0) {
        LOG_ERROR_RETURN(EINVAL, -1, "size or offset is not aligned to 4K, size : `, offset : `",
                         len, offset);
    }
    return cache_store_->evict(offset, len);
}

int CachedFile::fstat(struct stat *buf) {
    return src_file_ ? src_file_->fstat(buf) : -1;
}

int CachedFile::close() {
    if (src_file_) {
        return src_file_->close();
    }
    return 0;
}

ssize_t CachedFile::read(void *buf, size_t count) {
    struct iovec v {
        buf, count
    };
    return readv(&v, 1);
}

ssize_t CachedFile::readv(const struct iovec *iov, int iovcnt) {
    auto ret = preadv(iov, iovcnt, readOffset_);
    if (ret > 0) {
        readOffset_ += ret;
    }
    return ret;
}

ssize_t CachedFile::write(const void *buf, size_t count) {
    struct iovec v {
        const_cast<void *>(buf), count
    };
    return writev(&v, 1);
}

ssize_t CachedFile::writev(const struct iovec *iov, int iovcnt) {
    auto ret = pwritev(iov, iovcnt, writeOffset_);
    if (ret > 0) {
        writeOffset_ += ret;
    }
    return ret;
}

std::string_view CachedFile::get_pathname() {
    return get_store()->get_pathname();
}

ICachedFile *new_cached_file(IFile *src, ICacheStore *store, uint64_t pageSize, uint64_t refillUnit,
                             IOAlloc *allocator, IFileSystem *fs) {
    // new_cached_file requires src is able to fstat
    // once stat is failed, it will return nullptr
    struct stat st = {};
    if (src) {
        auto ok = src->fstat(&st);
        if (-1 == ok) {
            LOG_ERRNO_RETURN(0, nullptr, "src_file fstat failed : `", ok);
        }
    }
    return new CachedFile(src, store, st.st_size, pageSize, refillUnit, allocator, fs);
}

} //  namespace Cache
