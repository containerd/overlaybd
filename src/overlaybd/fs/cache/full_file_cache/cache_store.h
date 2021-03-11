/*
 * cache_store.h
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

#include <stddef.h>
#include <string>
#include "../../../range-lock.h"
#include "cache_pool.h"

namespace FileSystem {
class IFileSystem;
struct fiemap;
} // namespace FileSystem

namespace Cache {

class FileCachePool;

class FileCacheStore : public FileSystem::ICacheStore {
public:
    typedef FileCachePool::FileNameMap::iterator FileIterator;
    FileCacheStore(FileSystem::ICachePool *cachePool, IFile *localFile, size_t refillUnit,
                   FileIterator iterator);
    ~FileCacheStore();

    ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override;

    ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override;

    int stat(CacheStat *stat) override;
    int evict(off_t offset, size_t count = -1) override;

    std::pair<off_t, size_t> queryRefillRange(off_t offset, size_t size) override;

    int fstat(struct stat *buf) override;

protected:
    bool cacheIsFull();

    struct ReadRequest {
        off_t offset;
        size_t size;
    };

    //  merge from first extent to last extent(or encounter hole),
    //  because fiemap could return multiple continuous extents even though no any hole.
    std::pair<off_t, size_t> getFirstMergedExtents(struct fiemap *fie);

    std::pair<off_t, size_t> getLastMergedExtents(struct fiemap *fie);

    std::pair<off_t, off_t> getHoleFromCacheHitResult(off_t offset, size_t alignSize,
                                                      struct FileSystem::fiemap *fie);

    FileCachePool *cachePool_; //  owned by extern class
    IFile *localFile_;         //  owned by current class
    size_t refillUnit_;
    FileIterator iterator_;
    RangeLock rangeLock_;

    ssize_t do_pwritev(const struct iovec *iov, int iovcnt, off_t offset);
};

} //  namespace Cache
