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
#include <photon/photon.h>
#include <photon/fs/localfs.h>
#include <photon/fs/virtual-file.h>
#include <photon/fs/aligned-file.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

#include <sys/time.h>
#include <fcntl.h>
#include "../zfile.cpp"
#include "../compressor.cpp"
#include <memory>

#include <string>
#include <fstream>
#include <cstdlib>
#include <stdio.h>
#include <sched.h> //cpu_set_t , CPU_SET
#include <thread>
#include <stdio.h>
#include <chrono>

using namespace std;
using namespace photon::fs;
using namespace ZFile;

DEFINE_int32(nwrites, 16384, "write times in each layer.");
DEFINE_int32(log_level, 1, "log level");

class ZFileTest : public ::testing::Test {
public:
    unique_ptr<IFileSystem> lfs;

    int write_times = FLAGS_nwrites;

    void SetUp() {
        lfs.reset(new_localfs_adaptor("/tmp"));
    }

    void randwrite(IFile *file, int write_cnt) {
        LOG_INFO("write ` times.", write_cnt);
        while (write_cnt--) {
            int data[4096]{};
            for (ssize_t i = 0; i < (ssize_t)(sizeof(data) / sizeof(data[0])); i += 4)
                // data[i] = rand();
                data[i] = rand();
            // memset(data+i, rand(), 4);
            file->write(data, sizeof(data));
        }
        LOG_INFO("write done.");
    }

    void seqread(IFile *fsrc, IFile *fzfile) {
        LOG_INFO("start seqread.");
        struct stat _st;
        if (fsrc->fstat(&_st) != 0) {
            LOG_ERROR("err: `(`)", errno, strerror(errno));
            return;
        }
        auto size = _st.st_size;
        char data0[16384]{}, data1[16384]{};
        for (auto i = 0; i < size; i += sizeof(data0)) {
            fsrc->pread(data0, sizeof(data0), i);
            fzfile->pread(data1, sizeof(data1), i);
            auto r = memcmp(data0, data1, sizeof(data0));
            ASSERT_EQ(r, 0);
            if (r != 0) {
                LOG_ERROR("verify failed. offset: `", i);
                return;
            }
        }
    }

    void randread(IFile *fsrc, IFile *fzfile) {
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
        while (read_times--) {
            auto offset = rand() % counts;
            auto len = std::min(counts - offset, rand() % 32);
            if (len == 0)
                len = 1;
            fsrc->pread(data0, len * 512, offset * 512);
            fzfile->pread(data1, len * 512, offset * 512);
            auto r = memcmp(data0, data1, len * 512);
            ASSERT_EQ(r, 0);
            if (r != 0) {
                LOG_ERROR("verify failed. offset: `", offset * 512);
                return;
            }
        }
        char large_data0[ZFile::MAX_READ_SIZE << 1]{};
        char large_data1[ZFile::MAX_READ_SIZE << 1]{};
        LOG_INFO("start large read. (size: `K, 5K times)", (ZFile::MAX_READ_SIZE << 1) >> 10);
        for (int i = 0; i < 5000; i++) {
            auto len = sizeof(large_data0) / 512;
            auto offset = rand() % (counts - len);
            fsrc->pread(large_data0, len * 512, offset * 512);
            fzfile->pread(large_data1, len * 512, offset * 512);
            auto r = memcmp(large_data0, large_data1, len * 512);
            EXPECT_EQ(r, 0);
            if (r != 0) {
                LOG_ERROR("verify failed.");
                return;
            }
        }
    }
};

TEST_F(ZFileTest, reject_oversized_index) {
    const char *filename = "oversized-index.zfile";
    auto file = lfs->open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT_NE(file, nullptr);

    char ht_buf[CompressionFile::HeaderTrailer::SPACE]{};
    auto ht = new (ht_buf) CompressionFile::HeaderTrailer;
    CompressOptions opt;
    ht->set_compress_option(opt);
    ht->index_offset = CompressionFile::HeaderTrailer::SPACE;
    ht->index_size = MAX_ZFILE_INDEX_SIZE + 1;
    ASSERT_EQ(write_header_trailer(file, true, false, true, ht),
              (int)CompressionFile::HeaderTrailer::SPACE);

    const uint64_t trailer_offset =
        ht->index_offset + ht->index_size * sizeof(uint32_t);
    ASSERT_EQ(file->lseek(trailer_offset, SEEK_SET), (off_t)trailer_offset);
    ASSERT_EQ(write_header_trailer(file, false, true, true, ht),
              (int)CompressionFile::HeaderTrailer::SPACE);

    EXPECT_EQ(zfile_open_ro(file, false), nullptr);
    delete file;
    lfs->unlink(filename);
}

/*
testcases:
  checksum{disable, enable} x algorithm{lz4, zstd} x bs{4K, 8K, 16K, 32K, 64K}
*/
TEST_F(ZFileTest, verify_compression) {
    // log_output_level = 1;
    auto fn_src = "verify.data";
    auto fn_zfile = "verify.zfile";
    auto fn_dec = "verify.data.0";
    auto src = lfs->open(fn_src, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);
    unique_ptr<IFile> fsrc(src);
    if (!fsrc) {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
    }

    randwrite(fsrc.get(), write_times);
    struct stat _st;
    if (fsrc->fstat(&_st) != 0) {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
        return;
    }
    for (auto enable_crc = 0; enable_crc <= 1; enable_crc++) {
        for (auto algorithm = 1; algorithm <= 2; algorithm++) {
            for (auto bs = 12; bs <= 16; bs++ ) { // 4K ~ 64K
                auto dst = lfs->open(fn_zfile, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);
                auto dec = lfs->open(fn_dec, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);
                unique_ptr<IFile> fdst(dst);
                unique_ptr<IFile> fdec(dec);
                if (!fdst || !fdec) {
                    LOG_ERROR("err: `(`)", errno, strerror(errno));
                }
                CompressOptions opt;
                opt.algo = algorithm;
                opt.verify = enable_crc;
                opt.block_size = 1<<bs;
                CompressArgs args(opt);
                zfile_compress(fsrc.get(), nullptr, &args);
                fsrc->lseek(0, SEEK_SET);
                int ret = zfile_compress(fsrc.get(), fdst.get(), &args);
                auto fzfile = zfile_open_ro(fdst.get(), opt.verify);
                EXPECT_EQ(ret, 0);
                seqread(fsrc.get(), fzfile);
                randread(fsrc.get(), fzfile);
                ret = zfile_decompress(fdst.get(), fdec.get());
                EXPECT_EQ(ret, 0);
                EXPECT_EQ(is_zfile(fdec.get()), 0);
                LOG_INFO("start seqread.");
                auto size = _st.st_size;
                char data0[16384]{}, data1[16384]{};
                for (auto i = 0; i < size; i += sizeof(data0)) {
                    fsrc->pread(data0, sizeof(data0), i);
                    fdec->pread(data1, sizeof(data1), i);
                    if (memcmp(data0, data1, sizeof(data0)) != 0) {
                        LOG_ERROR("verify failed.");
                        return;
                    }
                }
            }
        }
    }
}

TEST_F(ZFileTest, validation_check) {
    // log_output_level = 1;
    auto fn_src = "verify.data";
    auto fn_zfile = "verify.zfile";
    auto src = lfs->open(fn_src, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);
    unique_ptr<IFile> fsrc(src);
    if (!fsrc) {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
    }
    randwrite(fsrc.get(), write_times);
    struct stat _st;
    if (fsrc->fstat(&_st) != 0) {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
        return;
    }
    auto dst = lfs->open(fn_zfile, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);
    unique_ptr<IFile> fdst(dst);
    if (!fdst) {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
    }
    CompressOptions opt;
    opt.algo = CompressOptions::LZ4;
    opt.verify = 1;
    CompressArgs args(opt);
    int ret = zfile_compress(fsrc.get(), fdst.get(), &args);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(zfile_validation_check(fdst.get()), 0);
    char error_data[8192];
    fdst->pwrite(error_data, 8192, 8192);
    EXPECT_NE(zfile_validation_check(fdst.get()), 0);
}

TEST_F(ZFileTest, ht_check) {
    // log_output_level = 1;
    auto fn_src = "verify.data";
    auto fn_zfile = "verify.zfile";
    auto src = lfs->open(fn_src, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);
    unique_ptr<IFile> fsrc(src);
    if (!fsrc) {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
    }
    randwrite(fsrc.get(), 1024);
    struct stat _st;
    if (fsrc->fstat(&_st) != 0) {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
        return;
    }
    auto dst = lfs->open(fn_zfile, O_CREAT | O_TRUNC | O_RDWR /*| O_DIRECT */, 0644);
    unique_ptr<IFile> fdst(dst);
    if (!fdst) {
        LOG_ERROR("err: `(`)", errno, strerror(errno));
    }
    CompressOptions opt;
    opt.algo = CompressOptions::LZ4;
    opt.verify = 1;
    CompressArgs args(opt);
    int ret = zfile_compress(fsrc.get(), fdst.get(), &args);
    EXPECT_EQ(ret, 0);
    auto x=2324;
    dst->pwrite(&x, sizeof(x), 400);
    EXPECT_NE(zfile_validation_check(fdst.get()), 0);
    EXPECT_EQ(is_zfile(dst), -1);
}

TEST_F(ZFileTest, dsa) {
    const int buf_size = 1024;
    const int crc_count = 3000;
    int ret = 0;

    for (auto i = 0; i < crc_count; i++) {
        void *buf = malloc(buf_size);
        DEFER(free(buf));
        uint32_t checksum_dsa = crc32::crc32c(buf, buf_size);
        uint32_t checksum_sse = crc32::testing::crc32c_fast(buf, buf_size, 0);
        if (checksum_dsa != checksum_sse) {
            ret = 1;
        }
    }

    ASSERT_EQ(ret, 0);
}

TEST_F(ZFileTest, verify_builder) {
    auto fn_src = "verify.data";
    auto fn_zfile = "verify.zfile";
    auto fn_zfile_1 = "verify.zfile.1";
    auto src = lfs->open(fn_src, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (src == nullptr) {
        LOG_ERROR("failed to open file: `(`)", errno, strerror(errno));
        return;
    }
    randwrite(src, write_times);
    struct stat _st;
    if (src->fstat(&_st) != 0) {
        LOG_ERROR("failed randwrite src file: `(`)", errno, strerror(errno));
        return;
    }

    // zfile builder multi-processor
    auto dst = lfs->open(fn_zfile, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (!dst) {
        LOG_ERROR("failed to open file: `(`)", errno, strerror(errno));
    }
    DEFER({delete dst;});
    ZFile::CompressOptions opt;
    opt.verify = 1;
    opt.block_size = 4096;
    ZFile::CompressArgs zfile_args(opt);
    zfile_args.workers = 4;
    auto zfile_builder = ZFile::new_zfile_builder(dst, &zfile_args, false);
    src->lseek(0, 0);
    char buf[16*1024];
    while (true) {
        auto sz = rand() % 8192 + 1;
        auto rc = src->read(buf, sz);
        if (rc <= 0) break;
        zfile_builder->write(buf, rc);
    }
    zfile_builder->close();

    // zfile builder
    ZFile::CompressOptions opt_1;
    opt_1.verify = 1;
    opt_1.block_size = 4096;
    ZFile::CompressArgs zfile_args_1(opt_1);
    zfile_args_1.workers = 1;
    auto dst_1 = lfs->open(fn_zfile_1, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (!dst_1) {
        LOG_ERROR("failed to open file: `(`)", errno, strerror(errno));
    }
    DEFER({delete dst_1;});
    auto zfile_builder_1 = ZFile::new_zfile_builder(dst_1, &zfile_args_1, false);
    src->lseek(0, 0);
    while (true) {
        auto sz = rand() % 8192 + 1;
        auto rc = src->read(buf, sz);
        if (rc <= 0) break;
        zfile_builder_1->write(buf, rc);
    }
    zfile_builder_1->close();

    EXPECT_EQ(dst->lseek(0, SEEK_CUR), dst_1->lseek(0, SEEK_CUR));
    dst->lseek(0, 0);
    dst_1->lseek(0, 0);
    char buf_1[16*1024];
    while (true) {
        auto rc = dst->read(buf, 8192);
        auto rc_1 = dst_1->read(buf_1, 8192);
        EXPECT_EQ(rc, rc_1);
        EXPECT_EQ(memcmp(buf, buf_1, rc), 0);
        if (rc == 0) break;
    }
}


TEST_F(ZFileTest, index_compression_roundtrip) {
    class ShortReadFile : public ForwardFile {
    public:
        explicit ShortReadFile(IFile *file) : ForwardFile(file) {}
        ssize_t read(void *buf, size_t count) override {
            return m_file->read(buf, std::min(count, size_t(3001)));
        }
    };
    enum class Writer { WholeFile, SingleWorker, MultiWorker, ShortReads };
    // All writers and both metadata paths, including an index larger than a data block.
    for (auto algo : {CompressOptions::LZ4, CompressOptions::ZSTD}) {
        for (auto writer : {Writer::WholeFile, Writer::SingleWorker,
                            Writer::MultiWorker, Writer::ShortReads}) {
            for (bool overwrite : {false, true}) {
                for (bool enabled : {false, true}) {
                    for (size_t size : {size_t(0), size_t(1), size_t(4096 * 128),
                                       size_t(4096 * 128 + 17), size_t(4096 * 2048 + 17)}) {
                        SCOPED_TRACE(::testing::Message() << "algo=" << int(algo)
                            << " writer=" << int(writer) << " overwrite=" << overwrite
                            << " enabled=" << enabled << " size=" << size);
                        auto src = unique_ptr<IFile>(lfs->open("index-src", O_CREAT | O_TRUNC | O_RDWR, 0644));
                        auto dst = unique_ptr<IFile>(lfs->open("index-dst", O_CREAT | O_TRUNC | O_RDWR, 0644));
                        ASSERT_NE(src, nullptr);
                        ASSERT_NE(dst, nullptr);
                        DEFER(lfs->unlink("index-src"); lfs->unlink("index-dst"););
                        std::vector<unsigned char> input(size);
                        for (size_t i = 0; i < size; ++i)
                            input[i] = (i % 4096) % 251;
                        if (size)
                            ASSERT_EQ(src->write(input.data(), size), (ssize_t)size);
                        ASSERT_EQ(src->lseek(0, SEEK_SET), 0);
                        CompressOptions opt(algo, 4096, 1);
                        CompressArgs args(opt);
                        args.compress_index = enabled;
                        args.overwrite_header = overwrite;
                        args.workers = writer == Writer::MultiWorker ? 2 : 1;
                        if (writer == Writer::WholeFile) {
                            ASSERT_EQ(zfile_compress(src.get(), dst.get(), &args), 0);
                        } else if (writer == Writer::ShortReads) {
                            ShortReadFile short_src(src.get());
                            ASSERT_EQ(zfile_compress(&short_src, dst.get(), &args), 0);
                        } else {
                            auto builder = unique_ptr<IFile>(new_zfile_builder(dst.get(), &args, false));
                            ASSERT_NE(builder, nullptr);
                            for (size_t offset = 0; offset < size;) {
                                size_t count = std::min(size - offset, size_t(3001));
                                ASSERT_EQ(builder->write(input.data() + offset, count), (ssize_t)count);
                                offset += count;
                            }
                            ASSERT_EQ(builder->close(), 0);
                        }
                        CompressionFile::HeaderTrailer ht;
                        CompressionFile::JumpTable table;
                        ASSERT_TRUE(load_jump_table(dst.get(), &ht, table));
                        EXPECT_EQ(ht.is_index_compressed(), enabled && size > 4096);
                        const size_t entries = size / 4096 + (size % 4096 != 0);
                        if (ht.is_index_compressed())
                            EXPECT_LT(ht.index_size, entries * sizeof(uint32_t));
                        else
                            EXPECT_EQ(ht.index_size, entries);
                        auto opened = unique_ptr<IFile>(zfile_open_ro(dst.get(), true, false));
                        ASSERT_NE(opened, nullptr);
                        struct stat st;
                        ASSERT_EQ(opened->fstat(&st), 0);
                        EXPECT_EQ(st.st_size, (off_t)size);
                        std::vector<unsigned char> output(size);
                        if (size) {
                            ASSERT_EQ(opened->pread(output.data(), size, 0), (ssize_t)size);
                            EXPECT_EQ(output, input);
                            // Deliberately unaligned reads spanning data-block boundaries.
                            for (size_t offset : {size_t(1), size_t(4090), size_t(8191)}) {
                                if (offset >= size) continue;
                                size_t count = std::min(size - offset, size_t(6013));
                                ASSERT_EQ(opened->pread(output.data(), count, offset), (ssize_t)count);
                                EXPECT_EQ(memcmp(output.data(), input.data() + offset, count), 0);
                            }
                        }
                        unsigned char byte;
                        EXPECT_EQ(opened->pread(&byte, 1, size), 0);
                    }
                }
            }
        }
    }
}

TEST_F(ZFileTest, index_compression_rejects_bad_metadata_and_payload) {
    for (auto algo : {CompressOptions::LZ4, CompressOptions::ZSTD}) {
        for (bool overwrite : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "algo=" << int(algo)
                         << " overwrite=" << overwrite);
            auto src = unique_ptr<IFile>(lfs->open("bad-index-src", O_CREAT | O_TRUNC | O_RDWR, 0644));
            auto dst = unique_ptr<IFile>(lfs->open("bad-index-dst", O_CREAT | O_TRUNC | O_RDWR, 0644));
            ASSERT_NE(src, nullptr);
            ASSERT_NE(dst, nullptr);
            DEFER(lfs->unlink("bad-index-src"); lfs->unlink("bad-index-dst"););
            std::vector<char> input(4096 * 128, 'a');
            ASSERT_EQ(src->write(input.data(), input.size()), (ssize_t)input.size());
            ASSERT_EQ(src->lseek(0, SEEK_SET), 0);
            CompressOptions opt(algo);
            CompressArgs args(opt);
            args.compress_index = true;
            args.overwrite_header = overwrite;
            ASSERT_EQ(zfile_compress(src.get(), dst.get(), &args), 0);
            struct stat st;
            ASSERT_EQ(dst->fstat(&st), 0);
            const off_t metadata_offset = overwrite ? 0 : st.st_size - 512;
            alignas(CompressionFile::HeaderTrailer) char original[512];
            ASSERT_EQ(dst->pread(original, sizeof(original), metadata_offset), 512);
            auto original_ht = (CompressionFile::HeaderTrailer *)original;
            ASSERT_TRUE(original_ht->is_index_compressed());
            CompressionFile::JumpTable table;
            ASSERT_TRUE(load_jump_table(dst.get(), nullptr, table));
            unsigned char first_byte;
            ASSERT_EQ(dst->pread(&first_byte, 1, original_ht->index_offset), 1);
            auto reject_metadata = [&](const char *description, auto mutate) {
                SCOPED_TRACE(description);
                alignas(CompressionFile::HeaderTrailer) char data[512];
                memcpy(data, original, sizeof(data));
                auto ht = (CompressionFile::HeaderTrailer *)data;
                mutate(*ht);
                ht->digest = 0;
                ht->digest = crc32::crc32c(data, sizeof(data));
                ASSERT_EQ(dst->pwrite(data, sizeof(data), metadata_offset), 512);
                EXPECT_FALSE(load_jump_table(dst.get(), nullptr, table));
            };
            reject_metadata("offset past EOF", [&](auto &ht) { ht.index_offset = st.st_size + 1; });
            reject_metadata("offset before data", [](auto &ht) { ht.index_offset = 511; });
            reject_metadata("index overlaps trailer", [&](auto &ht) { ht.index_size = st.st_size; });
            reject_metadata("too many entries", [](auto &ht) {
                ht.original_file_size = (MAX_ZFILE_INDEX_SIZE + 1) * 4096;
            });
            reject_metadata("more entries than data bytes", [](auto &ht) {
                ht.original_file_size = (ht.index_offset - 512 + 1) * 4096;
            });
            reject_metadata("decoded bytes exceed codec limit", [](auto &ht) {
                ht.original_file_size = ((uint64_t)INT_MAX / sizeof(uint32_t) + 1) * 4096;
            });
            reject_metadata("zero block size", [](auto &ht) { ht.opt.block_size = 0; });
            reject_metadata("non-power-of-two block size", [](auto &ht) { ht.opt.block_size = 3; });
            reject_metadata("unsupported block size", [](auto &ht) { ht.opt.block_size = MAX_READ_SIZE * 2; });
            reject_metadata("unsupported codec", [](auto &ht) { ht.opt.algo = CompressOptions::MINI_LZO; });
            reject_metadata("empty compressed index", [](auto &ht) { ht.index_size = 0; });
            reject_metadata("empty decoded index", [](auto &ht) { ht.original_file_size = 0; });
            reject_metadata("wrong decoded entry count", [](auto &ht) { ht.original_file_size += 4096; });
            reject_metadata("wrong index checksum", [](auto &ht) { ht.index_crc ^= 1; });
            reject_metadata("truncated codec stream with valid CRC", [&](auto &ht) {
                ht.index_size = 1;
                ht.index_crc = crc32::crc32c(&first_byte, 1);
            });

            // Metadata integrity must be checked before using any changed fields.
            alignas(CompressionFile::HeaderTrailer) char bad_digest[512];
            memcpy(bad_digest, original, sizeof(bad_digest));
            ((CompressionFile::HeaderTrailer *)bad_digest)->digest ^= 1;
            ASSERT_EQ(dst->pwrite(bad_digest, sizeof(bad_digest), metadata_offset), 512);
            EXPECT_FALSE(load_jump_table(dst.get(), nullptr, table));
            ASSERT_EQ(dst->pwrite(original, sizeof(original), metadata_offset), 512);
            first_byte ^= 0xff;
            ASSERT_EQ(dst->pwrite(&first_byte, 1, original_ht->index_offset), 1);
            EXPECT_FALSE(load_jump_table(dst.get(), nullptr, table));
        }
    }
}

TEST_F(ZFileTest, index_compression_rejects_inconsistent_raw_index) {
    for (bool overwrite : {false, true}) {
        auto src = unique_ptr<IFile>(lfs->open("raw-index-src", O_CREAT | O_TRUNC | O_RDWR, 0644));
        auto dst = unique_ptr<IFile>(lfs->open("raw-index-dst", O_CREAT | O_TRUNC | O_RDWR, 0644));
        ASSERT_NE(src, nullptr);
        ASSERT_NE(dst, nullptr);
        DEFER(lfs->unlink("raw-index-src"); lfs->unlink("raw-index-dst"););
        ASSERT_EQ(src->write("a", 1), 1);
        ASSERT_EQ(src->lseek(0, SEEK_SET), 0);
        CompressOptions opt;
        CompressArgs args(opt);
        args.compress_index = true; // A one-entry index must fall back to raw storage.
        args.overwrite_header = overwrite;
        ASSERT_EQ(zfile_compress(src.get(), dst.get(), &args), 0);
        CompressionFile::HeaderTrailer loaded;
        CompressionFile::JumpTable table;
        ASSERT_TRUE(load_jump_table(dst.get(), &loaded, table));
        ASSERT_FALSE(loaded.is_index_compressed());
        struct stat st;
        ASSERT_EQ(dst->fstat(&st), 0);
        const off_t metadata_offset = overwrite ? 0 : st.st_size - 512;
        alignas(CompressionFile::HeaderTrailer) char metadata[512];
        ASSERT_EQ(dst->pread(metadata, sizeof(metadata), metadata_offset), 512);
        auto ht = (CompressionFile::HeaderTrailer *)metadata;
        // The file claims two blocks but the raw index contains only one entry.
        ht->original_file_size = 8192;
        ht->digest = 0;
        ht->digest = crc32::crc32c(metadata, sizeof(metadata));
        ASSERT_EQ(dst->pwrite(metadata, sizeof(metadata), metadata_offset), 512);
        EXPECT_FALSE(load_jump_table(dst.get(), nullptr, table));
    }
}

TEST_F(ZFileTest, index_compression_checks_data_extent) {
    for (auto algo : {CompressOptions::LZ4, CompressOptions::ZSTD}) {
        for (bool compressed : {false, true}) {
            for (bool overwrite : {false, true}) {
                for (int adjustment : {-1, 0, 1}) {
                    SCOPED_TRACE(::testing::Message() << "algo=" << int(algo)
                        << " compressed=" << compressed << " overwrite=" << overwrite
                        << " adjustment=" << adjustment);
                    auto dst = unique_ptr<IFile>(lfs->open("index-extent", O_CREAT | O_TRUNC | O_RDWR, 0644));
                    ASSERT_NE(dst, nullptr);
                    DEFER(lfs->unlink("index-extent"););
                    alignas(CompressionFile::HeaderTrailer) char metadata[512]{};
                    auto ht = new (metadata) CompressionFile::HeaderTrailer;
                    ht->set_compress_option(CompressOptions(algo));
                    ASSERT_EQ(write_header_trailer(dst.get(), true, false, true, ht), 512);
                    // Only the index is read; the data area has a known total length.
                    std::vector<char> data(128 * 20);
                    ASSERT_EQ(dst->write(data.data(), data.size()), (ssize_t)data.size());
                    std::vector<uint32_t> lengths(128, 20);
                    lengths[0] = 20 + adjustment;
                    ASSERT_EQ(write_index(dst.get(), lengths, ht, 512 + data.size(), compressed), 0);
                    ASSERT_EQ(ht->is_index_compressed(), compressed);
                    ht->original_file_size = lengths.size() * 4096;
                    ASSERT_EQ(write_header_trailer(dst.get(), false, true, true, ht), 512);
                    if (overwrite)
                        ASSERT_EQ(write_header_trailer(dst.get(), true, false, true, ht, 0), 512);
                    CompressionFile::JumpTable table;
                    EXPECT_EQ(load_jump_table(dst.get(), nullptr, table), adjustment == 0);
                }
            }
        }
    }
}

TEST_F(ZFileTest, index_compression_rejects_oversized_builder_write) {
    for (int workers : {1, 2}) {
        auto dst = unique_ptr<IFile>(lfs->open("index-write-limit", O_CREAT | O_TRUNC | O_RDWR, 0644));
        ASSERT_NE(dst, nullptr);
        DEFER(lfs->unlink("index-write-limit"););
        CompressOptions opt;
        CompressArgs args(opt);
        args.compress_index = true;
        args.workers = workers;
        auto builder = unique_ptr<IFile>(new_zfile_builder(dst.get(), &args, false));
        ASSERT_NE(builder, nullptr);
        // The fixed limit is enforced before touching the caller's buffer or workers.
        const uint64_t max_input = MAX_ZFILE_INDEX_SIZE * opt.block_size;
        EXPECT_EQ(builder->write(nullptr, max_input + 1), -1);
        EXPECT_EQ(errno, EFBIG);
        ASSERT_EQ(builder->write("a", 1), 1);
        ASSERT_EQ(builder->close(), 0);
        auto opened = unique_ptr<IFile>(zfile_open_ro(dst.get(), false, false));
        ASSERT_NE(opened, nullptr);
        char byte;
        ASSERT_EQ(opened->pread(&byte, 1, 0), 1);
        EXPECT_EQ(byte, 'a');
    }
}

TEST_F(ZFileTest, index_compression_codec_boundaries) {
    for (auto algo : {CompressOptions::LZ4, CompressOptions::ZSTD}) {
        std::vector<unsigned char> encoded;
        EXPECT_EQ(compress_index_buffer(nullptr, 0, algo, encoded), 0);
        EXPECT_EQ(compress_index_buffer(nullptr, (size_t)INT_MAX + 1, algo, encoded), 0);
        uint32_t tiny = 123;
        EXPECT_EQ(compress_index_buffer(&tiny, sizeof(tiny), algo, encoded), 0);
        for (size_t count : {size_t(100), size_t(4096), size_t(32768)}) {
            std::vector<uint32_t> raw(count, 123), restored(count);
            ASSERT_EQ(compress_index_buffer(raw.data(), count * 4, algo, encoded), 1);
            EXPECT_EQ(decompress_index_buffer(encoded.data(), encoded.size(), restored.data(), count * 4, algo), 0);
            EXPECT_EQ(raw, restored);
            EXPECT_EQ(decompress_index_buffer(encoded.data(), encoded.size(), restored.data(), count * 4 - 4, algo), -1);
        }
    }
}

int main(int argc, char **argv) {
    auto seed = 154702356;
    cerr << "seed = " << seed << endl;
    srand(seed);

    ::testing::InitGoogleTest(&argc, argv);
    ::gflags::ParseCommandLineFlags(&argc, &argv, true);
    log_output_level = FLAGS_log_level;
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    auto ret = RUN_ALL_TESTS();
    return ret;
}
