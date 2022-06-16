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

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <random>
#include <algorithm>
#include <memory>

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/fs/aligned-file.h>
#include <photon/thread/thread.h>
#include <photon/common/io-alloc.h>

#include "../../cache.h"
#include "random_generator.h"

namespace Cache {

using namespace FileSystem;

// Cleanup and recreate the test dir
inline void SetupTestDir(const std::string &dir) {
    std::string cmd = std::string("rm -r ") + dir;
    system(cmd.c_str());
    cmd = std::string("mkdir -p ") + dir;
    system(cmd.c_str());
}

void commonTest(bool cacheIsFull, bool enableDirControl, bool dirFull) {
    std::string prefix = "";
    const size_t dirQuota = 32ul * 1024 * 1024;
    const uint64_t refillSize = 1024 * 1024;

    std::string root("/tmp/obdcache/cache_test/");
    SetupTestDir(root);

    std::string subDir = prefix + "dir/dir/";
    SetupTestDir(root + subDir);
    std::system(std::string("touch " + root + subDir + "testFile").c_str());

    struct stat st;
    auto ok = ::stat(std::string(root + subDir + "testFile").c_str(), &st);
    EXPECT_EQ(0, ok);

    std::string srcRoot("/tmp/obdcache/src_test/");
    SetupTestDir(srcRoot);
    auto srcFs = new_localfs_adaptor(srcRoot.c_str(), ioengine_psync);

    auto mediaFs = new_localfs_adaptor(root.c_str(), ioengine_libaio);
    auto alignFs = new_aligned_fs_adaptor(mediaFs, 4 * 1024, true, true);
    auto cacheAllocator = new AlignedAlloc(4 * 1024);
    auto roCachedFs = new_full_file_cached_fs(srcFs, alignFs, refillSize, cacheIsFull ? 0 : 512,
                                              1000 * 1000 * 1, 128ul * 1024 * 1024, cacheAllocator);
    auto cachePool = roCachedFs->get_pool();

    SetupTestDir(srcRoot + prefix + "testDir");
    auto srcFile = srcFs->open(std::string(prefix + "/testDir/file_1").c_str(),
                               O_RDWR | O_CREAT | O_TRUNC, 0644);

    UniformCharRandomGen gen(0, 255);
    off_t offset = 0;
    uint32_t kPageSize = 4 * 1024;
    uint32_t kFileSize = kPageSize * 16384; // 64MB
    uint32_t kPageCount = kFileSize / kPageSize;
    for (uint32_t i = 0; i < kPageCount; ++i) {
        std::vector<unsigned char> data;
        for (uint32_t j = 0; j < kPageSize; ++j) {
            data.push_back(gen.next());
        }
        srcFile->pwrite(data.data(), data.size(), offset);
        offset += kPageSize;
    }

    //  write some unaligned
    off_t lastOffset = offset;
    off_t unAlignedLen = 750;
    {
        std::vector<unsigned char> data;
        for (uint32_t j = 0; j < kPageSize; ++j) {
            data.push_back(gen.next());
        }
        srcFile->pwrite(data.data(), unAlignedLen, offset);
    }

    auto cachedFile = static_cast<ICachedFile *>(
        roCachedFs->open(std::string(prefix + "/testDir/file_1").c_str(), 0, 0644));

    //  test unaligned block
    {
        void *buf = malloc(kPageSize);
        auto ret = cachedFile->pread(buf, kPageSize, lastOffset);

        std::vector<unsigned char> src;
        src.reserve(kPageSize);
        auto retSrc = srcFile->pread(src.data(), kPageSize, lastOffset);

        EXPECT_EQ(0, std::memcmp(buf, src.data(), unAlignedLen));
        EXPECT_EQ(unAlignedLen, retSrc);
        EXPECT_EQ(unAlignedLen, ret);

        LOG_INFO("read again");

        // read again
        ret = cachedFile->pread(buf, kPageSize, lastOffset);
        EXPECT_EQ(unAlignedLen, ret);

        free(buf);
    }

    //  test aligned and unaligned block
    {
        void *buf = malloc(kPageSize * 4);
        auto ret = cachedFile->pread(buf, kPageSize * 4, lastOffset - 2 * kPageSize);

        std::vector<unsigned char> src;
        src.reserve(kPageSize * 4);
        auto retSrc = srcFile->pread(src.data(), kPageSize * 4, lastOffset - 2 * kPageSize);

        EXPECT_EQ(0, std::memcmp(buf, src.data(), 2 * kPageSize + unAlignedLen));
        EXPECT_EQ(2 * kPageSize + unAlignedLen, retSrc);
        EXPECT_EQ(2 * kPageSize + unAlignedLen, ret);

        LOG_INFO("read again");

        // read again
        ret = cachedFile->pread(buf, kPageSize * 4, lastOffset - 2 * kPageSize);
        EXPECT_EQ(2 * kPageSize + unAlignedLen, ret);

        free(buf);
    }

    std::vector<char> readBuf;
    readBuf.reserve(kPageSize);
    std::vector<char> readSrcBuf;
    readSrcBuf.reserve(kPageSize);
    for (int i = 0; i != 5; ++i) {
        EXPECT_EQ(kPageSize, cachedFile->read(readBuf.data(), kPageSize));
        srcFile->read(readSrcBuf.data(), kPageSize);
        EXPECT_EQ(0, std::memcmp(readBuf.data(), readSrcBuf.data(), kPageSize));
    }

    // test refill(3)
    if (!cacheIsFull) {
        auto inSrcFile = cachedFile->get_source();
        cachedFile->set_source(nullptr);
        struct stat stat;
        inSrcFile->fstat(&stat);
        cachedFile->ftruncate(stat.st_size);
        void *buf = malloc(kPageSize * 3);
        DEFER(free(buf));
        std::vector<char> src;
        src.reserve(kPageSize * 3);
        EXPECT_EQ(kPageSize, srcFile->pread(src.data(), kPageSize, 0));
        memcpy(buf, src.data(), kPageSize);

        EXPECT_EQ(kPageSize, cachedFile->refill(buf, kPageSize, 0));

        memset(buf, 0, kPageSize);
        EXPECT_EQ(kPageSize, cachedFile->pread(buf, kPageSize, 0));
        EXPECT_EQ(0, memcmp(buf, src.data(), kPageSize));

        struct stat st1;
        ::stat(std::string(root + prefix + "/file_1").c_str(), &st1);
        EXPECT_EQ(0, cachedFile->evict(0, kPageSize));
        struct stat st2;
        ::stat(std::string(root + prefix + "/file_1").c_str(), &st2);
        EXPECT_EQ(kPageSize, st1.st_blocks * 512 - st2.st_blocks * 512);

        // test refill last block
        src.clear();
        EXPECT_EQ(kPageSize + unAlignedLen,
                  srcFile->pread(src.data(), kPageSize * 3, lastOffset - kPageSize));
        memcpy(buf, src.data(), kPageSize * 3);
        EXPECT_EQ(kPageSize + unAlignedLen,
                  cachedFile->refill(buf, kPageSize * 3, lastOffset - kPageSize));
        memset(buf, 0, kPageSize * 3);
        EXPECT_EQ(kPageSize + unAlignedLen,
                  cachedFile->pread(buf, kPageSize * 3, lastOffset - kPageSize));
        EXPECT_EQ(0, memcmp(buf, src.data(), kPageSize + unAlignedLen));

        cachedFile->set_source(inSrcFile);
    }

    //  test refill(2)
    if (!cacheIsFull) {
        auto inSrcFile = cachedFile->get_source();

        void *buf = malloc(kPageSize * 2);
        DEFER(free(buf));
        EXPECT_EQ(2 * kPageSize, cachedFile->refill(kPageSize, 2 * kPageSize));

        cachedFile->set_source(nullptr);
        EXPECT_EQ(2 * kPageSize, cachedFile->pread(buf, 2 * kPageSize, kPageSize));
        std::vector<char> src;
        src.reserve(kPageSize * 2);
        EXPECT_EQ(kPageSize * 2, srcFile->pread(src.data(), 2 * kPageSize, kPageSize));
        EXPECT_EQ(0, memcmp(buf, src.data(), 2 * kPageSize));
        cachedFile->set_source(inSrcFile);

        // prefetch more than 16MB
        EXPECT_EQ(5000 * kPageSize + kPageSize, cachedFile->prefetch(234, 5000 * kPageSize));
        // prefetch tail
        EXPECT_EQ(kPageSize + unAlignedLen,
                  cachedFile->prefetch(lastOffset - kPageSize, 5000 * kPageSize));
    }

    if (dirFull) {
        CacheStat cstat = {};
        EXPECT_EQ(0, cachePool->stat(&cstat, prefix));
        EXPECT_EQ(dirQuota / refillSize, cstat.total_size);
    }

    // test aligned section
    UniformInt32RandomGen genOffset(0, (kPageCount + 1) * kPageSize);
    UniformInt32RandomGen genSize(0, 8 * kPageSize);
    struct stat srcSt = {};
    srcFile->fstat(&srcSt);
    for (int i = 0; i != 10000; ++i) {
        auto tmpOffset = genOffset.next();
        auto size = genSize.next();

        if (tmpOffset >= srcSt.st_size) {
            size = 0;
        } else {
            size = tmpOffset + size > srcSt.st_size ? srcSt.st_size - tmpOffset : size;
        }
        void *buf = malloc(size);
        auto ret = cachedFile->pread(buf, size, tmpOffset);

        std::vector<unsigned char> src;
        src.reserve(size);
        auto retSrc = srcFile->pread(src.data(), size, tmpOffset);

        EXPECT_EQ(0, std::memcmp(buf, src.data(), size));
        EXPECT_EQ(size, retSrc);
        EXPECT_EQ(size, ret);
        free(buf);
    }
    srcFile->close();

    photon::thread_usleep(1000 * 1000ull);
    ok = ::stat(std::string(root + subDir + "testFile").c_str(), &st);
    EXPECT_EQ(cacheIsFull || dirFull ? -1 : 0, ok);

    delete cachedFile;

    //  test smaller file
    {
        auto smallFile = srcFs->open(std::string(prefix + "/testDir/small").c_str(),
                                     O_RDWR | O_CREAT | O_TRUNC, 0644);
        DEFER(delete smallFile);
        int smallSize = 102;
        std::vector<char> smallData;
        for (int i = 0; i != smallSize; ++i) {
            smallData.push_back(gen.next());
        }
        EXPECT_EQ(smallSize, smallFile->pwrite(smallData.data(), smallData.size(), 0));

        auto smallCache = static_cast<ICachedFile *>(
            roCachedFs->open(std::string(prefix + "/testDir/small").c_str(), 0, 0644));
        DEFER(delete smallCache);

        void *sBuffer = malloc(kPageSize);
        DEFER(free(sBuffer));
        EXPECT_EQ(smallSize, smallCache->pread(sBuffer, kPageSize, 0));
        EXPECT_EQ(0, std::memcmp(sBuffer, smallData.data(), smallSize));

        memset(sBuffer, 0, kPageSize);
        EXPECT_EQ(smallSize, smallCache->pread(sBuffer, kPageSize, 0));
        EXPECT_EQ(0, std::memcmp(sBuffer, smallData.data(), smallSize));

        smallFile->close();
    }

    //  test refill
    {
        auto refillFile = srcFs->open(std::string(prefix + "/testDir/refill").c_str(),
                                      O_RDWR | O_CREAT | O_TRUNC, 0644);
        DEFER(delete refillFile);
        int refillSize = 4097;
        std::vector<char> refillData;
        for (int i = 0; i != refillSize; ++i) {
            refillData.push_back(gen.next());
        }
        EXPECT_EQ(refillSize, refillFile->pwrite(refillData.data(), refillData.size(), 0));

        auto refillCache = static_cast<ICachedFile *>(
            roCachedFs->open(std::string(prefix + "/testDir/refill").c_str(), 0, 0644));
        DEFER(delete refillCache);

        void *sBuffer = malloc(kPageSize * 2);
        DEFER(free(sBuffer));
        memset(sBuffer, 0, kPageSize * 2);
        EXPECT_EQ(kPageSize, refillCache->pread(sBuffer, kPageSize, 0));
        EXPECT_EQ(0, std::memcmp(sBuffer, refillData.data(), kPageSize));

        memset(sBuffer, 0, kPageSize * 2);
        EXPECT_EQ(refillSize, refillCache->pread(sBuffer, kPageSize * 2, 0));
        EXPECT_EQ(0, std::memcmp(sBuffer, refillData.data(), refillSize));

        refillFile->close();
    }

    delete srcFs;
    delete roCachedFs;
}

TEST(RoCachedFs, Basic) {
    commonTest(false, false, false);
}

TEST(RoCachedFs, BasicCacheFull) {
    commonTest(true, false, false);
}

TEST(RoCachedFs, CacheWithOutSrcFile) {
    std::string root("/tmp/obdcache/cache_test_no_src/");
    SetupTestDir(root);

    auto mediaFs = new_localfs_adaptor(root.c_str(), ioengine_libaio);
    auto alignFs = new_aligned_fs_adaptor(mediaFs, 4 * 1024, true, true);
    auto cacheAllocator = new AlignedAlloc(4 * 1024);
    DEFER(delete cacheAllocator);
    auto roCachedFs = new_full_file_cached_fs(nullptr, alignFs, 1024 * 1024, 512, 1000 * 1000 * 1,
                                              128ul * 1024 * 1024, cacheAllocator);
    DEFER(delete roCachedFs);
    auto cachedFile = static_cast<ICachedFile *>(
        roCachedFs->open(std::string("/testDir/file_1").c_str(), 0, 0644));
    DEFER(delete cachedFile);

    cachedFile->ftruncate(1024 * 1024);
    std::vector<char> buf;
    int len = 8 * 1024;
    buf.reserve(len);
    EXPECT_EQ(len, cachedFile->pwrite(buf.data(), len, 4 * 1024));
    EXPECT_EQ(len / 2, cachedFile->pread(buf.data(), 4 * 1024, 4 * 1024));
    EXPECT_EQ(-1, cachedFile->pread(buf.data(), len, 0));

    auto writeFile = static_cast<ICachedFile *>(
        roCachedFs->open(std::string("/testDir/file_2").c_str(), 0, 0644));
    DEFER(delete writeFile);
    writeFile->ftruncate(1024 * 1024);
    buf.assign(len, 'a');
    EXPECT_EQ(len, writeFile->write(buf.data(), len));
    EXPECT_EQ(len, writeFile->write(buf.data(), len));
    std::vector<char> res;
    res.reserve(len);
    EXPECT_EQ(len, writeFile->pread(res.data(), len, 0));
    EXPECT_EQ(0, std::memcmp(buf.data(), res.data(), len));
    res.assign(len, '0');
    EXPECT_EQ(len, writeFile->pread(res.data(), len, len));
    EXPECT_EQ(0, std::memcmp(buf.data(), res.data(), len));
    EXPECT_EQ(-1, writeFile->pread(res.data(), len, len * 2));
}

} //  namespace Cache

int main(int argc, char **argv) {
    log_output_level = 0;
    ::testing::InitGoogleTest(&argc, argv);

    photon::init(photon::INIT_EVENT_EPOLL|photon::INIT_IO_LIBCURL|photon::INIT_EVENT_SIGNALFD, photon::INIT_IO_LIBAIO);
    int ret = RUN_ALL_TESTS();
    return ret;
}
