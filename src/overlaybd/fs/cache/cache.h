/*
 * cache.h
 *
 * Copyright (C) 2021 Alibaba Group.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */
#pragma once
#include <inttypes.h>
#include <sys/uio.h>
#include <stdlib.h>
#include "../filesystem.h"
#include "pool_store.h"

struct IOAlloc;
namespace FileSystem {
class ICachedFileSystem : public IFileSystem {
public:
    // get the source file system
    UNIMPLEMENTED_POINTER(IFileSystem *get_source());

    // set the source file system
    UNIMPLEMENTED(int set_source(IFileSystem *src));

    UNIMPLEMENTED_POINTER(ICachePool *get_pool());
};

class ICachedFile : public IFile {
public:
    // get the source file system
    UNIMPLEMENTED_POINTER(IFile *get_source());

    // set the source file system, and enable `auto_refill`
    UNIMPLEMENTED(int set_source(IFile *src));

    UNIMPLEMENTED_POINTER(ICacheStore *get_store());

    // client refill for an ICachedFile (without a source!)
    // is implemented as pwrite(), usually aligned
    ssize_t refill(const void *buf, size_t count, off_t offset) {
        return pwrite(buf, count, offset);
    }
    ssize_t refill(const struct iovec *iov, int iovcnt, off_t offset) {
        return pwritev(iov, iovcnt, offset);
    }
    ssize_t refill(struct iovec *iov, int iovcnt, off_t offset) {
        return pwritev(iov, iovcnt, offset);
    }

    // refilling a range without providing data, is treated as prefeching
    ssize_t refill(off_t offset, size_t count) {
        return prefetch(offset, count);
    }

    // prefeching a range is implemented as reading the range without a buffer
    ssize_t prefetch(off_t offset, size_t count) {
        iovec iov{nullptr, count};
        return preadv(&iov, 1, offset);
    }

    // query cached extents is implemented as fiemap()
    UNIMPLEMENTED(int query(off_t offset, size_t count))

    // eviction is implemented as trim()
    ssize_t evict(off_t offset, size_t count) {
        return trim(offset, count);
    }

    int vioctl(int request, va_list args) override {
        auto src = get_source();
        if (src)
            return src->vioctl(request, args);
        return -1;
    }
};

class IMemCachedFile : public ICachedFile {
public:
    // Get the internal buffer for the specified LBA range (usually aligned),
    // which will remain valid for user until released by unpin_buffer().
    // Will allocate pages for missed ranges.
    // Will refill / fetch / load data from source if `refill`.
    // Concurrent R/W to a same range are guaranteed to work, but considered
    // a race-condition and the result is undefiend.
    // returns # of bytes actually got, or <0 for failures
    virtual ssize_t pin_buffer(off_t offset, size_t count, bool refill, /*OUT*/ iovector *iov) = 0;

    // Release buffers got from pin_buffer(),
    // and the buffer is no longer valid for user.
    // return 0 for success, < 0 for failures
    virtual int unpin_buffer(off_t offset, const iovector *iov) = 0;
};

extern "C" {
ICachedFileSystem *new_cached_fs(IFileSystem *src, ICachePool *pool, uint64_t pageSize,
                                 uint64_t refillUnit, IOAlloc *allocator);

ICachedFileSystem *new_full_file_cached_fs(IFileSystem *srcFs, IFileSystem *media_fs,
                                           uint64_t refillUnit, uint64_t capacityInGB,
                                           uint64_t periodInUs, uint64_t diskAvailInBytes,
                                           IOAlloc *allocator);

ICachedFile *new_mem_cached_file(IFile *src, uint64_t mem_size, uint64_t refillUnit,
                                 IOAlloc *allocator);

ICachedFileSystem *new_mem_cached_fs(IFileSystem *src, uint64_t mem_size, uint64_t refillUnit,
                                     IOAlloc *allocator);

ICachedFileSystem *new_block_cached_fs(IFileSystem *srcFs, uint64_t refillUnit,
                                       uint64_t memoryCapacityInGB, uint64_t diskCapacityInGB,
                                       IOAlloc *allocator, bool shm, bool ipc = false);

IFileSystem *new_short_circuit_fs(IFileSystem *syncFs, const std::string &params);

IFileSystem *new_read_ahead_fs(IFileSystem *externFs, bool ownership = true,
                               size_t ahead = 2 * 1024 * 1024);
} // extern "C"

} // namespace FileSystem
