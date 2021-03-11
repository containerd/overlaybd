/*
 * cache_pool.h
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

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../../photon/thread.h"
#include "../../../photon/timer.h"
#include "../../../string-keyed.h"
#include "../policy/lru.h"
#include "../pool_store.h"

namespace FileSystem {
class IFileSystem;
class IFile;
} // namespace FileSystem
namespace Cache {

using namespace FileSystem;

class FileCachePool : public ICachePool {
public:
    FileCachePool(IFileSystem *mediaFs, uint64_t capacityInGB, uint64_t periodInUs,
                  uint64_t diskAvailInBytes, uint64_t refillUnit);
    ~FileCachePool();

    static const uint64_t kDiskBlockSize = 512; // stat(2)
    static const uint64_t kDeleteDelayInUs = 1000;
    static const uint32_t kWaterMarkRatio = 90;

    void Init();

    //  pathname must begin with '/'
    ICacheStore *do_open(std::string_view pathname, int flags, mode_t mode) override;

    int stat(CacheStat *stat, std::string_view pathname = std::string_view(nullptr, 0)) override;

    int evict(std::string_view filename) override;
    int evict(size_t size = 0) override;

    struct LruEntry {
        LruEntry(uint32_t lruIt, int openCnt, uint64_t fileSize)
            : lruIter(lruIt), openCount(openCnt), size(fileSize) {
        }
        ~LruEntry() = default;
        uint32_t lruIter;
        int openCount;
        uint64_t size;
        photon::rwlock rw_lock_;
    };

    // Normally, fileIndex(std::map) always keep growing, so its iterators always
    // keep valid, iterator will be erased when file be unlinked in period of eviction,
    // but on that time corresponding CachedFile had been destructed, so nobody hold
    // erased iterator.
    typedef map_string_key<std::unique_ptr<LruEntry>> FileNameMap;

    bool isFull();
    void removeOpenFile(FileNameMap::iterator iter);
    void forceRecycle();
    void updateLru(FileNameMap::iterator iter);
    uint64_t updateSpace(FileNameMap::iterator iter, uint64_t size);

protected:
    IFile *openMedia(std::string_view name, int flags, int mode);

    static uint64_t timerHandler(void *data);
    virtual void eviction();
    uint64_t calcWaterMark(uint64_t capacity, uint64_t maxFreeSpace);

    IFileSystem *mediaFs_; //  owned by current class
    uint64_t capacityInGB_;
    uint64_t periodInUs_;
    uint64_t diskAvailInBytes_;
    size_t refillUnit_;
    int64_t totalUsed_;
    int64_t riskMark_;
    uint64_t waterMark_;

    photon::Timer *timer_;
    bool running_;
    bool exit_;

    bool isFull_;

    virtual bool afterFtrucate(FileNameMap::iterator iter);

    int traverseDir(const std::string &root);
    virtual int insertFile(std::string_view file);

    typedef LRU<FileNameMap::iterator, uint32_t> LRUContainer;
    LRUContainer lru_;
    // filename -> lruEntry
    FileNameMap fileIndex_;
};

} //  namespace Cache