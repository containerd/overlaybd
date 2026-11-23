#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/fs/extfs/extfs.h>
#include <photon/fs/fiemap.h>
#include <unistd.h>
#include <vector>
#include <gtest/gtest.h>
#include <string>
#include "../overlaybd/gzip/gz.h"
#include "../overlaybd/tar/libtar.h"
#include "../overlaybd/tar/erofs/liberofs.h"
#include "../prefetch.cpp"

#include "../tools/comm_func.h"
#include "../tools/sha256file.h"
#include "photon/common/utility.h"
#include <photon/fs/aligned-file.h>

using namespace std;

const string test_urls[]={"https://github.com/containerd/overlaybd/archive/refs/tags/v1.0.12.tar.gz"};
const vector<pair<string, string>> fnlist = {
    {"1M", "sha256:4e29ad18ab9f42d7c233500771a39d7c852b200baf328fd00fbbe3fecea1eb56"},
    {"overlaybd-1.0.12/README.md", "sha256:4d4ca22ffdcdced61c121b2961fe24dd0a256f1e37bd866cbbbf02f6a0da0f2c"},
    {"overlaybd-1.0.12/docs/assets/Scaling_up.jpg", "sha256:d941365f9d087e106dbd7ff804eac19ef362347cd7069ffaad8f84cb12317ee7"},
    {"overlaybd-1.0.12/src/image_file.h", "sha256:cb98584c50c031c3c3c08d1fc03ad05d733d57a31ec32249a8d1e5150f352528"},
    {"overlaybd-1.0.12/src/version.h", "sha256:5b216e936c66e971292ff720a4843d9a03bca13d5a8b5dd393c7bedca592ca73"},
    {"overlaybd-1.0.12/src/main.cpp", "sha256:4653edf45471096d549b2a002d2b10dafb5beb939cff3f1dfc936fb4d75c070a"}
};

const string erofs_imgs[]={"https://github.com/salvete/erofs-imgs/raw/main/alpine.img"};
const vector<pair<string, string>> erofs_fnlist = {
    {"/bin/busybox", "sha256:42de297577993675efecf295acf0260e26128458048b3081451e1ac43f611b49"},
    {"/bin/sh", "sha256:42de297577993675efecf295acf0260e26128458048b3081451e1ac43f611b49"},
    {"/lib/ld-musl-x86_64.so.1", "sha256:60d0ed88672b260b8337bf1e5b721f9ca9c877f4d901886472b8195a38ff3630"},
    {"/lib/libc.musl-x86_64.so.1", "sha256:60d0ed88672b260b8337bf1e5b721f9ca9c877f4d901886472b8195a38ff3630"},
    {"/lib/libz.so.1.3.1", "sha256:5134dcc47a23d1bfa7cd0f8046343e9268d4d3f1827dce295713d3b10ada5e0a"},
    {"/lib/libz.so.1", "sha256:5134dcc47a23d1bfa7cd0f8046343e9268d4d3f1827dce295713d3b10ada5e0a"}
};

class MockFile : public ForwardFile_Ownership{
public:
    char trash_data[1<<20];
    MockFile(IFile *file) : ForwardFile_Ownership(file, true) {}
    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {
        if (buf == nullptr) {
            return m_file->pread(trash_data, count,offset);
        }
        return m_file->pread(buf, count, offset);
    }
};

int download(const std::string &url, const std::string &out) {
    // if (::access(out.c_str(), 0) == 0)
    //     return 0;
    auto base = std::string(basename(url.c_str()));
    // download file
    if (::access(out.c_str(), 0) == 0) return 0;
    std::string cmd = "curl -sL -o " + out + " " + url;
    LOG_INFO(VALUE(cmd.c_str()));
    auto ret = system(cmd.c_str());
    if (ret != 0) {
        LOG_ERRNO_RETURN(0, -1, "download failed: `", url.c_str());
    }
    return 0;
}

int download_erofs_img(const std::string &url, const std::string &out)
{
    std::string cmd = "wget -O" + out + " " + url;
    auto ret = system(cmd.c_str());
    if (ret != 0)
        LOG_ERRNO_RETURN(0, -1, "download failed: `", url.c_str());
    return 0;
}

namespace {

using Trace = PrefetcherImpl::TraceFormat;
using Header = PrefetcherImpl::TraceHeader;
using Op = Prefetcher::TraceOp;

class ReplayTestFile : public ForwardFile_Ownership {
public:
    vector<pair<size_t, off_t>> reads;
    bool fail_stat = false;
    bool negative_size = false;

    explicit ReplayTestFile(IFile *file) : ForwardFile_Ownership(file, true) {}

    ssize_t pread(void *buf, size_t count, off_t offset) override {
        reads.emplace_back(count, offset);
        if (count > PrefetcherImpl::MAX_IO_SIZE) {
            ADD_FAILURE() << "Replay read exceeds buffer capacity";
            errno = EINVAL;
            return -1;
        }
        return m_file->pread(buf, count, offset);
    }

    int fstat(struct stat *st) override {
        if (fail_stat) {
            errno = EIO;
            return -1;
        }
        auto ret = m_file->fstat(st);
        if (ret == 0 && negative_size) {
            st->st_size = -1;
        }
        return ret;
    }
};

class ShortTraceFile : public ForwardFile_Ownership {
public:
    int read_calls = 0;
    bool fail_read;

    ShortTraceFile(IFile *file, bool fail)
        : ForwardFile_Ownership(file, true), fail_read(fail) {}

    ssize_t read(void *buf, size_t count) override {
        if (++read_calls == 3) {
            if (fail_read) {
                errno = EIO;
                return -1;
            }
            return m_file->read(buf, count - 1);
        }
        return m_file->read(buf, count);
    }
};

class BinaryTraceTest : public ::testing::Test {
protected:
    PrefetcherImpl prefetch{1};
    unique_ptr<ReplayTestFile> source;
    string trace_path;
    string source_path;

    IFile *temporary_file(string &path) {
        char name[] = "/tmp/overlaybd-binary-trace-XXXXXX";
        int fd = mkstemp(name);
        if (fd < 0) {
            return nullptr;
        }
        close(fd);
        path = name;
        return open_localfile_adaptor(name, O_RDWR);
    }

    void SetUp() override {
        prefetch.m_trace_file = temporary_file(trace_path);
        ASSERT_NE(nullptr, prefetch.m_trace_file);
        auto file = temporary_file(source_path);
        ASSERT_NE(nullptr, file);
        source.reset(new ReplayTestFile(file));
        ASSERT_EQ(0, file->ftruncate(2 * PrefetcherImpl::MAX_IO_SIZE));
        prefetch.register_src_file(0, source.get());
    }

    void TearDown() override {
        if (!trace_path.empty()) {
            unlink(trace_path.c_str());
        }
        if (!source_path.empty()) {
            unlink(source_path.c_str());
        }
    }

    size_t write_trace(const vector<Trace> &records) {
        Header hdr = {};
        hdr.magic = PrefetcherImpl::TRACE_MAGIC;
        hdr.data_size = records.size() * sizeof(Trace);
        for (const auto &trace : records) {
            hdr.checksum = crc32::crc32c_extend(&trace, sizeof(trace), hdr.checksum);
        }
        EXPECT_EQ(0, prefetch.m_trace_file->ftruncate(0));
        EXPECT_EQ(0, prefetch.m_trace_file->lseek(0, SEEK_SET));
        EXPECT_EQ(static_cast<ssize_t>(sizeof(hdr)),
                  prefetch.m_trace_file->write(&hdr, sizeof(hdr)));
        for (const auto &trace : records) {
            EXPECT_EQ(static_cast<ssize_t>(sizeof(trace)),
                      prefetch.m_trace_file->write(&trace, sizeof(trace)));
        }
        EXPECT_EQ(0, prefetch.m_trace_file->lseek(0, SEEK_SET));
        return sizeof(hdr) + hdr.data_size;
    }

    void expect_rejected(size_t file_size) {
        EXPECT_EQ(-1, prefetch.reload(file_size));
        EXPECT_TRUE(prefetch.m_replay_queue.empty());
        EXPECT_EQ(0, prefetch.replay_worker_thread());
        EXPECT_TRUE(source->reads.empty());
    }
};

TEST_F(BinaryTraceTest, ReplaysCapacityAndValidRecords) {
    auto size = write_trace({
        {Op::READ, 0, PrefetcherImpl::MAX_IO_SIZE, PrefetcherImpl::MAX_IO_SIZE},
        {Op::READ, 0, 4096, 0},
        {Op::WRITE, 0, 4096, 0},
        {Op::READ, 1, 4096, 0},
    });
    ASSERT_EQ(0, prefetch.reload(size));
    ASSERT_EQ(4U, prefetch.m_replay_queue.size());
    ASSERT_EQ(0, prefetch.replay_worker_thread());
    ASSERT_EQ(2U, source->reads.size());
    EXPECT_EQ(make_pair(size_t(PrefetcherImpl::MAX_IO_SIZE),
                       off_t(PrefetcherImpl::MAX_IO_SIZE)), source->reads[0]);
    EXPECT_EQ(make_pair(size_t(4096), off_t(0)), source->reads[1]);
}

TEST_F(BinaryTraceTest, RejectsInvalidRecordsWithoutPartialReplay) {
    const Trace invalid[] = {
        {Op::READ, 0, 0, 0},
        {Op::READ, 0, PrefetcherImpl::MAX_IO_SIZE + 1, 0},
        {Op::READ, 0, numeric_limits<size_t>::max(), 0},
        {Op::READ, 0, 4096, -1},
        {Op::READ, 0, 1, numeric_limits<off_t>::max()},
        {Op::READ, 0, PrefetcherImpl::MAX_IO_SIZE,
         numeric_limits<off_t>::max() - PrefetcherImpl::MAX_IO_SIZE + 1},
        {static_cast<Op>('?'), 0, 4096, 0},
    };
    for (const auto &trace : invalid) {
        SCOPED_TRACE(::testing::Message() << "count=" << trace.count
                                         << " offset=" << trace.offset);
        auto size = write_trace({{Op::READ, 0, 4096, 0}, trace});
        expect_rejected(size);
    }
}

TEST_F(BinaryTraceTest, AcceptsRepresentableRangeEnd) {
    auto size = write_trace({
        {Op::READ, 0, PrefetcherImpl::MAX_IO_SIZE,
         numeric_limits<off_t>::max() - PrefetcherImpl::MAX_IO_SIZE},
    });
    EXPECT_EQ(0, prefetch.reload(size));
    EXPECT_EQ(1U, prefetch.m_replay_queue.size());
    EXPECT_EQ(0, prefetch.replay_worker_thread());
    EXPECT_TRUE(source->reads.empty());
}

TEST_F(BinaryTraceTest, RejectsMalformedHeaders) {
    auto size = write_trace({{Op::READ, 0, 4096, 0}});
    Header hdr = {};
    hdr.magic = PrefetcherImpl::TRACE_MAGIC;
    hdr.data_size = numeric_limits<size_t>::max() - sizeof(Header) + 2;
    ASSERT_EQ(static_cast<ssize_t>(sizeof(hdr)),
              prefetch.m_trace_file->pwrite(&hdr, sizeof(hdr), 0));
    expect_rejected(1);

    size = write_trace({{Op::READ, 0, 4096, 0}});
    hdr.data_size = sizeof(Trace) + 1;
    ASSERT_EQ(static_cast<ssize_t>(sizeof(hdr)),
              prefetch.m_trace_file->pwrite(&hdr, sizeof(hdr), 0));
    ASSERT_EQ(0, prefetch.m_trace_file->ftruncate(size + 1));
    expect_rejected(size + 1);

    size = write_trace({{Op::READ, 0, 4096, 0}});
    expect_rejected(size - 1);

    size = write_trace({});
    hdr.magic = 0;
    hdr.data_size = 0;
    ASSERT_EQ(static_cast<ssize_t>(sizeof(hdr)),
              prefetch.m_trace_file->pwrite(&hdr, sizeof(hdr), 0));
    expect_rejected(size);
}

TEST_F(BinaryTraceTest, RejectsChecksumWithoutPartialReplay) {
    auto size = write_trace({{Op::READ, 0, 4096, 0}});
    Header hdr = {};
    ASSERT_EQ(static_cast<ssize_t>(sizeof(hdr)),
              prefetch.m_trace_file->pread(&hdr, sizeof(hdr), 0));
    hdr.checksum ^= 1;
    ASSERT_EQ(static_cast<ssize_t>(sizeof(hdr)),
              prefetch.m_trace_file->pwrite(&hdr, sizeof(hdr), 0));
    expect_rejected(size);
}

TEST_F(BinaryTraceTest, RejectsIncompleteReadsWithoutPartialReplay) {
    auto size = write_trace({{Op::READ, 0, 4096, 0}, {Op::READ, 0, 4096, 4096}});
    ASSERT_EQ(0, prefetch.m_trace_file->ftruncate(sizeof(Header) - 1));
    expect_rejected(size);

    size = write_trace({{Op::READ, 0, 4096, 0}, {Op::READ, 0, 4096, 4096}});
    ASSERT_EQ(0, prefetch.m_trace_file->ftruncate(size - 1));
    expect_rejected(size);

    for (bool fail : {false, true}) {
        size = write_trace({{Op::READ, 0, 4096, 0}, {Op::READ, 0, 4096, 4096}});
        prefetch.m_trace_file = new ShortTraceFile(prefetch.m_trace_file, fail);
        expect_rejected(size);
    }
}

TEST_F(BinaryTraceTest, FailedReloadClearsPreviousQueue) {
    auto size = write_trace({{Op::READ, 0, 4096, 0}});
    ASSERT_EQ(0, prefetch.reload(size));
    ASSERT_FALSE(prefetch.m_replay_queue.empty());
    ASSERT_EQ(0, prefetch.m_trace_file->lseek(0, SEEK_SET));
    expect_rejected(size - 1);
}

TEST_F(BinaryTraceTest, ConstructorDisablesInvalidTrace) {
    write_trace({{Op::READ, 0, 4096, 0},
                 {Op::READ, 0, PrefetcherImpl::MAX_IO_SIZE + 1, 0}});
    unique_ptr<Prefetcher> loaded(new_prefetcher(trace_path, 1));
    ASSERT_NE(nullptr, loaded);
    EXPECT_EQ(Prefetcher::Mode::Disabled, loaded->get_mode());
    EXPECT_EQ(-1, loaded->replay());
    auto impl = static_cast<PrefetcherImpl *>(loaded.get());
    EXPECT_TRUE(impl->m_replay_queue.empty());
}

TEST_F(BinaryTraceTest, ConstructorReplaysValidTrace) {
    write_trace({{Op::READ, 0, PrefetcherImpl::MAX_IO_SIZE, 0}});
    unique_ptr<Prefetcher> loaded(new_prefetcher(trace_path, 2));
    ASSERT_NE(nullptr, loaded);
    ASSERT_EQ(Prefetcher::Mode::Replay, loaded->get_mode());
    auto impl = static_cast<PrefetcherImpl *>(loaded.get());
    impl->register_src_file(0, source.get());
    ASSERT_EQ(0, loaded->replay());
    ASSERT_NE(nullptr, impl->m_replay_thread);
    photon::thread_join(impl->m_replay_thread);
    impl->m_replay_thread = nullptr;
    ASSERT_EQ(1U, source->reads.size());
    EXPECT_EQ(make_pair(size_t(PrefetcherImpl::MAX_IO_SIZE), off_t(0)), source->reads[0]);
}

TEST_F(BinaryTraceTest, ReplayIndependentlyChecksBounds) {
    prefetch.m_replay_queue.push({Op::READ, 0, 0, 0});
    prefetch.m_replay_queue.push({Op::READ, 0, PrefetcherImpl::MAX_IO_SIZE + 1, 0});
    prefetch.m_replay_queue.push({Op::READ, 0, 4096, -1});
    prefetch.m_replay_queue.push({Op::READ, 0, 1, numeric_limits<off_t>::max()});
    prefetch.m_replay_queue.push({static_cast<Op>('?'), 0, 4096, 0});
    prefetch.m_replay_queue.push({Op::READ, 0, 4096, 2 * PrefetcherImpl::MAX_IO_SIZE});
    prefetch.m_replay_queue.push({Op::READ, 0, 0, 2 * PrefetcherImpl::MAX_IO_SIZE + 1});
    prefetch.m_replay_queue.push({Op::READ, 0, 4096, 2 * PrefetcherImpl::MAX_IO_SIZE - 1});
    prefetch.m_replay_queue.push({Op::READ, 0, PrefetcherImpl::MAX_IO_SIZE, 0});
    ASSERT_EQ(0, prefetch.replay_worker_thread());
    ASSERT_EQ(1U, source->reads.size());
    EXPECT_EQ(make_pair(size_t(PrefetcherImpl::MAX_IO_SIZE), off_t(0)), source->reads[0]);
}

TEST_F(BinaryTraceTest, ReplaySkipsUnavailableLayerSize) {
    source->fail_stat = true;
    prefetch.m_replay_queue.push({Op::READ, 0, 4096, 0});
    ASSERT_EQ(0, prefetch.replay_worker_thread());
    EXPECT_TRUE(source->reads.empty());
}

TEST_F(BinaryTraceTest, ReplaySkipsNegativeLayerSize) {
    source->negative_size = true;
    prefetch.m_replay_queue.push({Op::READ, 0, 4096, 0});
    ASSERT_EQ(0, prefetch.replay_worker_thread());
    EXPECT_TRUE(source->reads.empty());
}

TEST_F(BinaryTraceTest, MissingTraceFileFailsReload) {
    delete prefetch.m_trace_file;
    prefetch.m_trace_file = nullptr;
    expect_rejected(sizeof(Header));
}

TEST_F(BinaryTraceTest, EmptyTraceIsValid) {
    auto size = write_trace({});
    EXPECT_EQ(0, prefetch.reload(size));
    EXPECT_TRUE(prefetch.m_replay_queue.empty());
}

} // namespace

TEST(trace, case0) {

    return;

    std::string workdir = "/tmp/trace_test/";
    mkdir(workdir.c_str(), 0755);
    auto dst = open_file((workdir+"img").c_str(), O_CREAT|O_RDWR|O_TRUNC, 0644);
    dst = new MockFile(dst);
    DEFER(delete dst);
    dst->ftruncate(32 << 20);

    auto flist = open_file((workdir+"list").c_str(), O_CREAT|O_RDWR|O_TRUNC, 0644);
    for (auto it : fnlist) {
        auto fn = it.first + "\n";
        flist->write(fn.c_str(), fn.size());
    }
    flist->write("overlaybd-1.0.12/src/\n", strlen("overlaybd-1.0.12/src/\n"));
    delete flist;

    std::string fn_test_tgz = workdir + basename(test_urls[0].c_str());
    ASSERT_EQ(
        0, download(test_urls[0], fn_test_tgz.c_str()));
    auto src = open_gzfile_adaptor(fn_test_tgz.c_str());
    ASSERT_NE(src, nullptr);
    // src = open_gzfile_adaptor(src);
    DEFER(delete src);
    auto aligned_file = new_aligned_file_adaptor(dst, 4096, true, false);
    ASSERT_NE(aligned_file, nullptr);
    DEFER(delete aligned_file);
    auto fs = create_ext4fs(aligned_file, true, false);
    ASSERT_NE(fs, nullptr);
    DEFER(delete fs);
    auto tar = new UnTar(src, fs, 0, 4096, nullptr, false);
    ASSERT_EQ(tar->extract_all(), 0);
    auto file = fs->open("/1M", O_TRUNC|O_CREAT|O_RDWR, 0644);

    ASSERT_NE(file, nullptr);
    char buf[1<<20];
    memset(buf, 'A', sizeof(buf));
    file->pwrite(buf, 1<<20, 0);
    delete file;

    auto p = new_dynamic_prefetcher((workdir+"list"), 8);
    p->replay(dst);

    for (auto fn : fnlist) {
        vector<pair<off_t, uint64_t>> lba;
        auto file = fs->open(fn.first.c_str(), O_RDONLY);
        auto fout = open_file((workdir+"check").c_str(), O_CREAT|O_RDWR|O_TRUNC, 0644);
        ASSERT_NE(file, nullptr);
        DEFER(delete file);
        struct stat buf;
        file->fstat(&buf);
        uint64_t size = buf.st_size;

        photon::fs::fiemap_t<8192> fie(0, size);
        ASSERT_EQ(file->fiemap(&fie), 0);
        LOG_INFO("check ` size: `, extents: `", fn.first, size, fie.fm_mapped_extents);
        // uint64_t count = ((size+ T_BLOCKSIZE - 1) / T_BLOCKSIZE) * T_BLOCKSIZE;
        for (uint32_t i = 0; i < fie.fm_mapped_extents; i++) {
            LOG_INFO("get segment: ` `", fie.fm_extents[i].fe_physical, fie.fm_extents[i].fe_length);
            lba.push_back(make_pair(fie.fm_extents[i].fe_physical, fie.fm_extents[i].fe_length < size ? fie.fm_extents[i].fe_length : size));
            size -= lba.back().second;
        }
        for (auto it : lba) {
            char data[4<<20];
            LOG_INFO("write segment: ` `", it.first, it.second);
            dst->pread(data, it.second, it.first);
            fout->write(data, it.second);
        }
        fout->lseek(0, SEEK_SET);
        auto sha256file = new_sha256_file(fout, true);
        DEFER(delete sha256file);
        LOG_INFO("verify sha256 of `", fn.first);
        ASSERT_STREQ(sha256file->sha256_checksum().c_str(), fn.second.c_str());
    }
    delete p;
}

TEST(trace, case1) {
    //return;
    std::string workdir = "/tmp/trace_test/";
    mkdir(workdir.c_str(), 0755);

    std::string erofs_img_path = workdir + basename(erofs_imgs[0].c_str());
    ASSERT_EQ(
        0, download_erofs_img(erofs_imgs[0], erofs_img_path.c_str()));

    auto dst = open_file(erofs_img_path.c_str(), O_RDONLY, 0644);
    dst = new MockFile(dst);
    DEFER(delete dst);

    auto flist = open_file((workdir+"list").c_str(), O_CREAT|O_RDWR|O_TRUNC, 0644);
    for (auto it : erofs_fnlist) {
        auto fn = it.first + "\n";
        flist->write(fn.c_str(), fn.size());
    }
    flist->write("/etc/\n", strlen("/etc/\n"));
    delete flist;

    auto fs = create_erofs_fs(dst, 4096);
    ASSERT_NE(fs, nullptr);
    DEFER(delete fs);

    auto p = new_dynamic_prefetcher(workdir+"list", 8);
    p->replay(dst);

    for (auto fn: erofs_fnlist) {
        vector<pair<off_t, uint64_t>> lba;
        auto file = fs->open(fn.first.c_str(), O_RDONLY);
        auto fout = open_file((workdir+"check").c_str(), O_CREAT|O_RDWR|O_TRUNC, 0644);
        LOG_INFO("file_name: `", fn.first.c_str());
        ASSERT_NE(file, nullptr);
        DEFER(delete file);
        struct stat buf;
        file->fstat(&buf);
        uint64_t size = buf.st_size;

        photon::fs::fiemap_t<8192> fie(0, size);
        ASSERT_EQ(file->fiemap(&fie), 0);
        LOG_INFO("check ` size: `, extents: `", fn.first, size, fie.fm_mapped_extents);
        // uint64_t count = ((size+ T_BLOCKSIZE - 1) / T_BLOCKSIZE) * T_BLOCKSIZE;
        for (uint32_t i = 0; i < fie.fm_mapped_extents; i++) {
            LOG_INFO("get segment: ` `", fie.fm_extents[i].fe_physical, fie.fm_extents[i].fe_length);
            lba.push_back(make_pair(fie.fm_extents[i].fe_physical, fie.fm_extents[i].fe_length < size ? fie.fm_extents[i].fe_length : size));
            size -= lba.back().second;
        }
        for (auto it : lba) {
            char data[4<<20];
            LOG_INFO("write segment: ` `", it.first, it.second);
            dst->pread(data, it.second, it.first);
            fout->write(data, it.second);
        }
        fout->lseek(0, SEEK_SET);
        auto sha256file = new_sha256_file(fout, true);
        DEFER(delete sha256file);
        LOG_INFO("verify sha256 of `", fn.first);
        ASSERT_STREQ(sha256file->sha256_checksum().c_str(), fn.second.c_str());
    }

    delete p;
}



int main(int argc, char **arg) {
    photon::init();
    DEFER(photon::fini());

    set_log_output_level(ALOG_INFO);
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}