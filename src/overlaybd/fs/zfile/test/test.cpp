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
#include <vector>
#include <iostream>
#include <gtest/gtest.h>
#include <gflags/gflags.h>
#include "../../localfs.h"
#include "../../virtual-file.h"
#include <sys/time.h>
#include <fcntl.h>
#include "../zfile.cpp"
#include "../compressor.cpp"
#include "../../aligned-file.h"
#include "../../../photon/thread11.h"
#include "../../../photon/syncio/fd-events.h"
#include "../../../photon/syncio/aio-wrapper.h"
#include <memory>

#include <optional>
#include <string>
#include <fstream>
#include <cstdlib>
#include <stdio.h>
#include <sched.h> //cpu_set_t , CPU_SET
#include <thread>
#include <stdio.h>
#include <chrono>

using namespace std;
using namespace FileSystem;
using namespace ZFile;

DEFINE_int32(nwrites, 16384, "write times in each layer.");
DEFINE_int32(log_level, 1, "log level");

class ZFileTest : public ::testing::Test 
{
public:
    unique_ptr<IFileSystem> lfs;

    int write_times = FLAGS_nwrites;

    void SetUp()
    {
        lfs.reset(new_localfs_adaptor("/tmp"));
    }

    void randwrite(IFile *file, int write_cnt)
    {
        LOG_INFO("write ` times.", write_cnt);
        while (write_cnt--)
        {
            int data[4096]{};
            for (ssize_t i = 0; i < (ssize_t)(sizeof(data) / sizeof(data[0])); i += 4)
                // data[i] = rand();
                data[i] = rand();
            // memset(data+i, rand(), 4);
            file->write(data, sizeof(data));
        }
        LOG_INFO("write done.");
    }

    void seqread(IFile *fsrc, IFile *fzfile)
    {
        LOG_INFO("start seqread.");
        struct stat _st;
        if (fsrc->fstat(&_st) != 0)
        {
            LOG_ERROR("err: `(`)", errno, strerror(errno));
            return;
        }
        auto size = _st.st_size;
        char data0[16384]{}, data1[16384]{};
        for (auto i = 0; i < size; i += sizeof(data0))
        {
            fsrc->pread(data0, sizeof(data0), i);
            fzfile->pread(data1, sizeof(data1), i);
            auto r = memcmp(data0, data1, sizeof(data0));
            EXPECT_EQ(r, 0);
            if (r != 0)
            {
                LOG_ERROR("verify failed. offset: `", i);
                return;
            }
        }
    }

    void randread(IFile *fsrc, IFile *fzfile)
    {
        int read_times = 1000;
        LOG_INFO("start randread. (` times)", read_times);
        struct stat _st;
        if (fsrc->fstat(&_st) != 0) {
            LOG_ERROR("err: `(`)", errno, strerror(errno));
            return;
        }
        auto size = _st.st_size;
        int counts = size / 512;
        char data0[16384]{}, data1[16384]{};
        while (read_times--)
        {
            auto offset = rand() % counts;
            auto len = std::min(counts - offset, rand() % 32);
            if (len == 0)
                len = 1;
            fsrc->pread(data0, len * 512, offset * 512);
            fzfile->pread(data1, len * 512, offset * 512);
            auto r = memcmp(data0, data1, len * 512);
            EXPECT_EQ(r, 0);
            if (r != 0)
            {
                LOG_ERROR("verify failed. offset: `", offset * 512);
                return;
            }
        }
        char large_data0[ZFile::MAX_READ_SIZE << 1]{};
        char large_data1[ZFile::MAX_READ_SIZE << 1]{};
        LOG_INFO("start large read. (size: `K, 5K times)", (ZFile::MAX_READ_SIZE << 1) >> 10);
        for (int i = 0; i < 5000; i++)
        {
            auto len = sizeof(large_data0) / 512;
            auto offset = rand() % (counts - len);
            fsrc->pread(large_data0, len * 512, offset * 512);
            fzfile->pread(large_data1, len * 512, offset * 512);
            auto r = memcmp(large_data0, large_data1, len * 512);
            EXPECT_EQ(r, 0);
            if (r != 0)
            {
                LOG_ERROR("verify failed.");
                return;
            }
        }
    }
};

TEST_F(ZFileTest, verify_lz4)
{
    // log_output_level = 1;
    auto fn_src = "verify.data";
    auto fn_lz4 = "verify.zlz4";
    unique_ptr<IFile> fsrc(lfs->open(fn_src, O_CREAT | O_TRUNC | O_RDWR, 0644));
    unique_ptr<IFile> fdst(lfs->open(fn_lz4, O_CREAT | O_TRUNC | O_RDWR, 0644));
    if (!fsrc || !fdst)
    {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
    }
    randwrite(fsrc.get(), write_times);
    CompressOptions opt;
    opt.verify = 1;
    CompressArgs args(opt);
    if (zfile_compress(fsrc.get(), fdst.get(), &args) != 0)
    {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
        return;
    }
    fdst->close();
    auto file = lfs->open(fn_lz4, O_RDONLY /*| O_DIRECT*/);
    auto faligned_dst = file;
    // auto faligned_dst = new_aligned_file_adaptor(file, ALIGNMENT_4K, true);
    IFile *flz4 = zfile_open_ro(faligned_dst, /*verify=*/true, false);
    EXPECT_EQ(is_zfile(faligned_dst), 1);
    DEFER(flz4->close());
    seqread(fsrc.get(), flz4);
    randread(fsrc.get(), flz4);
}

TEST_F(ZFileTest, verify_compression)
{
    // log_output_level = 1;
    auto fn_src = "verify.data";
    auto fn_lz4 = "verify.zlz4";
    auto fn_dec = "verify.data.0";
    auto src = lfs->open(fn_src, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);
    auto dst = lfs->open(fn_lz4, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);
    auto dec = lfs->open(fn_dec, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);
    unique_ptr<IFile> fsrc(src);
    // unique_ptr<IFile> fdst(new_aligned_file_adaptor(dst, ALIGNMENT_4K, true));
    unique_ptr<IFile> fdst(dst);
    unique_ptr<IFile> fdec(dec);
    if (!fsrc || !fdst || !fdec)
    {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
    }
    randwrite(fsrc.get(), write_times);
    CompressOptions opt;
    CompressArgs args(opt);
    zfile_compress(fsrc.get(), nullptr, &args);
    int ret = zfile_compress(fsrc.get(), fdst.get(), &args);
    EXPECT_EQ(ret, 0);
    ret = zfile_decompress(fdst.get(), fdec.get());
    EXPECT_EQ(ret, 0);
    // int read_times = 100000;
    EXPECT_EQ(is_zfile(fdec.get()), 0);
    LOG_INFO("start seqread.");
    struct stat _st;
    if (fsrc->fstat(&_st) != 0)
    {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
        return;
    }
    auto size = _st.st_size;
    char data0[16384]{}, data1[16384]{};
    for (auto i = 0; i < size; i += sizeof(data0))
    {
        fsrc->pread(data0, sizeof(data0), i);
        fdec->pread(data1, sizeof(data1), i);
        if (memcmp(data0, data1, sizeof(data0)) != 0)
        {
            LOG_ERROR("verify failed.");
            return;
        }
    }
}

TEST_F(ZFileTest, checksum)
{
    // log_output_level = 0;
    auto fn_src = "verify.data";
    auto fn_lz4 = "verify.zlz4";
    auto fn_dec = "verify.data.0";
    auto src = lfs->open(fn_src, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);
    auto dst = lfs->open(fn_lz4, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);
    auto dec = lfs->open(fn_dec, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);

    randwrite(src, write_times);
    CompressOptions opt;
    opt.verify = 1;
    CompressArgs args(opt);
    int ret = zfile_compress(src, dst, &args);
    EXPECT_EQ(ret, 0);
    ret = zfile_decompress(dst, dec);
    EXPECT_EQ(ret, 0);
    // int read_times = 100000;
    EXPECT_EQ(is_zfile(dec), 0);
    LOG_INFO("start seqread.");
    struct stat _st;
    if (src->fstat(&_st) != 0)
    {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
        return;
    }
    auto size = _st.st_size;
    char data0[16384]{}, data1[16384]{};
    for (auto i = 0; i < size; i += sizeof(data0))
    {
        auto readn0 = src->pread(data0, sizeof(data0), i);
        auto readn1 = dec->pread(data1, sizeof(data1), i);
        ASSERT_EQ(readn0, (ssize_t)sizeof(data0));
        ASSERT_EQ(readn1, (ssize_t)sizeof(data1));
        auto r = memcmp(data0, data1, sizeof(data0));
        ASSERT_EQ(r, 0);
    }
}

TEST_F(ZFileTest, dsa)
{
    const int buf_size = 1024;
    const int crc_count = 3000;
    int ret = 0;

    for (auto i = 0; i < crc_count; i++)
    {
        void *buf = malloc(buf_size);
        DEFER(free(buf));
        uint32_t checksum_dsa = FileSystem::crc32::crc32c(buf, buf_size);
        uint32_t checksum_sse = FileSystem::crc32::testing::crc32c_fast(buf, buf_size, 0);
        if (checksum_dsa != checksum_sse)
        {
            ret = 1;
        }
    }

    ASSERT_EQ(ret, 0);
}

int main(int argc, char **argv)
{
    auto seed = 154702356;
    cerr << "seed = " << seed << endl;
    srand(seed);

    ::testing::InitGoogleTest(&argc, argv);
    ::gflags::ParseCommandLineFlags(&argc, &argv, true);
    log_output_level = FLAGS_log_level;
    photon::init();
    photon::fd_events_epoll_init();
    photon::libaio_wrapper_init();
    auto ret = RUN_ALL_TESTS();
    photon::libaio_wrapper_fini();
    photon::fd_events_epoll_fini();
    photon::fini();
    return ret;
}
