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

#include <algorithm>
#include <array>
#include <memory>
#include <set>
#include <tuple>
#include <gtest/gtest.h>
#include <photon/fs/forwardfs.h>
#include <photon/fs/localfs.h>
#include "../tools/comm_func.h"
#include "../overlaybd/tar/libtar.h"

namespace {

using namespace photon::fs;
using File = std::unique_ptr<IFile>;
using FileSystem = std::unique_ptr<IFileSystem>;
using Mappings = std::vector<std::tuple<uint64_t, uint32_t, uint64_t>>;
constexpr size_t block_size = 4096;
constexpr size_t cache_size = 128UL << 20;
const std::array<size_t, 7> file_sizes = {{4095, 4096, 4097, 131071, 131072,
                                        131073, (130UL << 20) + 37}};

// 统计 extfs 缓存之下的实际 I/O，避免把 tar 的逻辑大小当作淘汰证据。
class ObservedFile : public ForwardFile {
public:
    explicit ObservedFile(IFile *file) : ForwardFile(file) {}
    std::set<off_t> read_blocks;
    size_t writes = 0;

    ssize_t pread(void *buf, size_t count, off_t offset) override {
        if (count == block_size && offset % block_size == 0)
            read_blocks.insert(offset);
        return m_file->pread(buf, count, offset);
    }

    ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        ++writes;
        return m_file->pwrite(buf, count, offset);
    }
};

class Ext4BufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        char path[] = "ext4-buffer-XXXXXX";
        ASSERT_NE(nullptr, mkdtemp(path));
        directory = path;
        source = create("layer.tar");
        index = create("image.idx");
        metadata = create("image.meta");
        headers = create("layer.headers");
        ASSERT_NE(nullptr, source);
        ASSERT_NE(nullptr, index);
        ASSERT_NE(nullptr, metadata);
        ASSERT_NE(nullptr, headers);
        ASSERT_NO_FATAL_FAILURE(write_tar());
    }

    void TearDown() override {
        headers.reset();
        metadata.reset();
        index.reset();
        source.reset();
        for (const auto &path : paths)
            EXPECT_EQ(0, ::unlink(path.c_str()));
        if (!directory.empty())
            EXPECT_EQ(0, ::rmdir(directory.c_str()));
    }

    File create(const char *name) {
        auto path = directory + "/" + name;
        File file(open_localfile_adaptor(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600));
        if (file)
            paths.push_back(path);
        return file;
    }

    static unsigned char pattern(size_t member, size_t offset) {
        return (offset + member * 17) % 251;
    }

    void write_tar() {
        for (size_t i = 0; i < file_sizes.size(); ++i) {
            TarHeader h;
            memset(&h, 0, T_BLOCKSIZE);
            snprintf(h.name, sizeof(h.name), "f%zu", i);
            memcpy(h.magic, TMAGIC, TMAGLEN);
            memcpy(h.version, TVERSION, TVERSLEN);
            h.typeflag = REGTYPE;
            int_to_oct(0644, h.mode, sizeof(h.mode));
            int_to_oct(0, h.uid, sizeof(h.uid));
            int_to_oct(0, h.gid, sizeof(h.gid));
            int_to_oct(file_sizes[i], h.size, sizeof(h.size));
            int_to_oct(0, h.mtime, sizeof(h.mtime));
            memset(h.chksum, ' ', sizeof(h.chksum));
            auto raw = reinterpret_cast<unsigned char *>(&h);
            unsigned int sum = 0;
            for (size_t j = 0; j < T_BLOCKSIZE; ++j)
                sum += raw[j];
            int_to_oct(sum, h.chksum, sizeof(h.chksum) - 1);
            ASSERT_EQ(T_BLOCKSIZE, source->write(raw, T_BLOCKSIZE));
            std::array<unsigned char, 64 * 1024> buf;
            for (size_t off = 0; off < file_sizes[i];) {
                auto n = std::min(buf.size(), file_sizes[i] - off);
                auto padded = (n + T_BLOCKSIZE - 1) / T_BLOCKSIZE * T_BLOCKSIZE;
                std::fill(buf.begin(), buf.end(), 0);
                for (size_t j = 0; j < n; ++j)
                    buf[j] = pattern(i, off + j);
                ASSERT_EQ(static_cast<ssize_t>(padded), source->write(buf.data(), padded));
                off += n;
            }
        }
        char end[2 * T_BLOCKSIZE] = {};
        ASSERT_EQ(static_cast<ssize_t>(sizeof(end)), source->write(end, sizeof(end)));
        ASSERT_EQ(0, source->lseek(0, SEEK_SET));
    }

    static Mappings remote_mappings(LSMT::IFileRW *image) {
        auto idx = image->index();
        auto entries = idx->dump();
        DEFER(free(entries));
        Mappings result;
        for (size_t i = 0; i < idx->size(); ++i) {
            const auto &m = entries[i];
            if (m.tag == static_cast<uint8_t>(LSMT::SegmentType::remoteData))
                result.emplace_back(static_cast<uint64_t>(m.offset),
                                    static_cast<uint32_t>(m.length),
                                    static_cast<uint64_t>(m.moffset));
        }
        return result;
    }

    void verify(IFileSystem *fs) {
        std::array<unsigned char, block_size> actual, expected;
        for (size_t i = 0; i < file_sizes.size(); ++i) {
            const auto name = "/f" + std::to_string(i);
            SCOPED_TRACE(name);
            File file(fs->open(name.c_str(), O_RDONLY));
            ASSERT_NE(nullptr, file);
            struct stat st;
            ASSERT_EQ(0, file->fstat(&st));
            ASSERT_EQ(static_cast<off_t>(file_sizes[i]), st.st_size);
            // 单次读取 4 KiB，确保大文件经过 buffer，而非大块直读旁路。
            for (size_t off = 0; off < file_sizes[i]; off += block_size) {
                auto n = std::min(block_size, file_sizes[i] - off);
                ASSERT_EQ(static_cast<ssize_t>(n), file->pread(actual.data(), n, off));
                for (size_t j = 0; j < n; ++j)
                    expected[j] = pattern(i, off + j);
                ASSERT_EQ(0, memcmp(actual.data(), expected.data(), n)) << "offset=" << off;
            }
            // 显式跨越 4 KiB 及旧 128 KiB cache block 边界。
            for (size_t boundary : {block_size, size_t(128 << 10)}) {
                const auto off = boundary - 17;
                if (off >= file_sizes[i])
                    continue;
                auto n = std::min(size_t(34), file_sizes[i] - off);
                ASSERT_EQ(static_cast<ssize_t>(n), file->pread(actual.data(), n, off));
                for (size_t j = 0; j < n; ++j)
                    expected[j] = pattern(i, off + j);
                ASSERT_EQ(0, memcmp(actual.data(), expected.data(), n)) << "boundary=" << boundary;
            }
        }
    }

    void check_conversion(bool import_headers) {
        IFile *input = source.get();
        if (import_headers) {
            UnTar dump(source.get(), nullptr, 0);
            ASSERT_EQ(static_cast<ssize_t>(file_sizes.size()), dump.dump_tar_headers(headers.get()));
            ASSERT_EQ(0, headers->lseek(0, SEEK_SET));
            input = headers.get();
        }

        // 转换时不提供 payload：大块缓存的读改写会读到零并覆盖 RemoteData 映射。
        LSMT::WarpFileArgs args(index.get(), metadata.get(), nullptr);
        args.virtual_size = 512UL << 20;
        std::unique_ptr<LSMT::IFileRW> image(LSMT::create_warpfile(args, false));
        ASSERT_NE(nullptr, image);
        Mappings mappings;
        {
            FileSystem fs(create_ext4fs(image.get(), true));
            UnTar tar(input, fs.get(), import_headers ? TAR_IGNORE_CRC : 0,
                      block_size, image.get(), true, import_headers);
            ASSERT_EQ(0, tar.extract_all());
            mappings = remote_mappings(image.get());
            ASSERT_FALSE(mappings.empty());
        }
        ASSERT_EQ(mappings, remote_mappings(image.get()));
        image.reset();

        // 重开后才接入真实 payload；制造脏目录块，再以 >128 MiB 的缓存读取淘汰它。
        image.reset(LSMT::open_warpfile_rw(index.get(), metadata.get(), source.get(), false));
        ASSERT_NE(nullptr, image);
        ASSERT_EQ(mappings, remote_mappings(image.get()));
        {
            ObservedFile observed(image.get());
            FileSystem fs(create_ext4fs(&observed, false));
            ASSERT_EQ(0, fs->mkdir("/eviction", 0755));
            observed.read_blocks.clear();
            observed.writes = 0;
            ASSERT_NO_FATAL_FAILURE(verify(fs.get()));
            ASSERT_GT(observed.read_blocks.size(), cache_size / block_size);
            // 读取阶段没有显式 sync；此处的回写证明脏缓存块确实被淘汰。
            ASSERT_GT(observed.writes, 0UL);
        }
        ASSERT_EQ(mappings, remote_mappings(image.get()));
        image.reset();

        // 丢弃所有 extfs/warp 缓存后再次验证持久化映射与完整文件内容。
        image.reset(LSMT::open_warpfile_rw(index.get(), metadata.get(), source.get(), false));
        ASSERT_NE(nullptr, image);
        ASSERT_EQ(mappings, remote_mappings(image.get()));
        FileSystem fs(create_ext4fs(image.get(), false));
        ASSERT_NO_FATAL_FAILURE(verify(fs.get()));
        struct stat st;
        ASSERT_EQ(0, fs->stat("/eviction", &st));
        ASSERT_TRUE(S_ISDIR(st.st_mode));
    }

    std::string directory;
    std::vector<std::string> paths;
    File source, index, metadata, headers;
};

TEST_F(Ext4BufferTest, turbo_conversion) {
    check_conversion(false);
}

TEST_F(Ext4BufferTest, tar_metadata_import) {
    check_conversion(true);
}

} // namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT) != 0)
        return 1;
    DEFER(photon::fini());
    set_log_output_level(ALOG_WARN);
    return RUN_ALL_TESTS();
}
