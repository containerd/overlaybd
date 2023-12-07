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

#include "../gzfile.h"
#include "../../gzip/gz.h"
#include "../../cache/gzip_cache/cached_fs.h"
#include "../../cache/cache.h"
#include <photon/photon.h>
#include <photon/common/io-alloc.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/fs/localfs.h>
#include <photon/io/fd-events.h>
#include <photon/net/socket.h>
#include <photon/thread/thread11.h>
// #include <photon/net/
#include <gtest/gtest.h>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>
#include <vector>
#include <cstring>
#include "../../../tools/sha256file.h"

struct PreadTestCase {
    off_t offset;
    size_t count;
    ssize_t ret;
};

class GzIndexTest : public ::testing::Test {
protected:
    static photon::fs::IFile *defile;
    static photon::fs::IFile *gzfile;
    static const size_t vsize = 10 << 20;

    virtual void SetUp() override {
    }
    virtual void TearDown() override {
    }

    static void SetUpTestSuite() {
        lfs = photon::fs::new_localfs_adaptor("/tmp");
        if (buildDataFile() != 0) {
            LOG_ERROR("failed to build ` and `", fn_defile, fn_gzdata);
            exit(-1);
        }
        if (buildIndexFile() != 0) {
            LOG_ERROR("failed to build gz index: `", fn_gzindex);
            exit(-1);
        }
        gzfile = new_gzfile(gzdata, gzindex);
        if (gzfile == nullptr) {
            LOG_ERROR("failed to new_gzfile(...)");
            exit(-1);
        }
    }

    static void TearDownTestSuite() {
        delete gzdata;
        delete gzindex;
        delete defile;
        delete gzfile;

        if (lfs->access(fn_defile, 0) == 0) {
            lfs->unlink(fn_defile);
        }
        if (lfs->access(fn_gzdata, 0) == 0) {
            lfs->unlink(fn_gzdata);
        }
        if (lfs->access(fn_gzindex, 0) == 0) {
            lfs->unlink(fn_gzindex);
        }
        delete lfs;
    }

    void test_pread(PreadTestCase t) {
        char *buf1 = new char[t.count];
        char *buf2 = new char[t.count];
        DEFER(delete[] buf1);
        DEFER(delete[] buf2);
        ssize_t ret1 = defile->pread(buf1, t.count, t.offset);
        ssize_t ret2 = gzfile->pread(buf2, t.count, t.offset);
        EXPECT_EQ(ret1, t.ret);
        EXPECT_EQ(ret2, t.ret);
        if (t.ret > 0) {
            EXPECT_EQ(strncmp(buf1, buf2, t.ret), 0);
        }
        LOG_DEBUG("pread testcase: { offset: `, count: `, ret: ` }", t.offset, t.count, t.ret);
    }

    void group_test_pread(std::vector<PreadTestCase> &t) {
        size_t testcases = t.size();
        LOG_INFO("Testing pread, ` sets of test cases ...", testcases);
        for (size_t i = 0; i < testcases; i++) {
            test_pread(t[i]);
        }
    }

private:
    static photon::fs::IFileSystem *lfs;
    static photon::fs::IFile *gzdata;
    static photon::fs::IFile *gzindex;

    static const char *fn_defile;
    static const char *fn_gzdata;
    static const char *fn_gzindex;

    static int buildDataFile() {
        // uncompressed data
        unsigned char *buf = new unsigned char[vsize];
        DEFER(delete[] buf);
        for (size_t i = 0; i < vsize; i++) {
            auto j = rand() % 256;
            buf[i] = j;
        }
        defile = lfs->open(fn_defile, O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (defile == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "failed to create `", fn_defile);
        }
        if (defile->pwrite(buf, vsize, 0) != vsize) {
            LOG_ERRNO_RETURN(0, -1, "failed to pwrite `", fn_defile);
        }
        // gzip data
        size_t gzlen = compressBound(vsize) + 4096;
        unsigned char *gzbuf = new unsigned char[gzlen];
        DEFER(delete[] gzbuf);
        if (gzip_compress(buf, vsize, gzbuf, gzlen) != 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to gzip_compress(...)");
        }
        gzdata = lfs->open(fn_gzdata, O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (gzdata == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "failed to create `", fn_gzdata);
        }
        if (gzdata->pwrite(gzbuf, gzlen, 0) != (ssize_t)gzlen) {
            LOG_ERRNO_RETURN(0, -1, "failed to pwrite `", fn_gzdata);
        }
        return 0;
    }

    static int buildIndexFile() {
        std::string fn_gzindex_path = std::string("/tmp/") + fn_gzindex;
        if (create_gz_index(gzdata, fn_gzindex_path.c_str()) != 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to create gz index: `", fn_gzindex);
        }
        gzindex = lfs->open(fn_gzindex, O_RDONLY, 0444);
        if (gzindex == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "failed to open gz index: `", fn_gzindex);
        }
        return 0;
    }

    static int gzip_compress(unsigned char *in, size_t in_len, unsigned char *out,
                             size_t &out_len) {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        int ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8,
                               Z_DEFAULT_STRATEGY);
        if (ret != Z_OK) {
            LOG_ERRNO_RETURN(0, -1, "failed to deflateInit2(...)");
        }
        strm.next_in = in;
        strm.avail_in = in_len;
        strm.next_out = out;
        strm.avail_out = out_len;
        ret = deflate(&strm, Z_FINISH);
        if (ret != Z_STREAM_END) {
            LOG_ERRNO_RETURN(0, -1, "ret of deflate(Z_FINISH) != Z_STREAM_END, ret:`", ret);
        }
        out_len -= strm.avail_out;
        LOG_INFO("uncompressed len: `, gzip len: `", in_len, out_len);
        return 0;
    }
};

photon::fs::IFileSystem *GzIndexTest::lfs = nullptr;
photon::fs::IFile *GzIndexTest::defile = nullptr;
photon::fs::IFile *GzIndexTest::gzfile = nullptr;
photon::fs::IFile *GzIndexTest::gzdata = nullptr;
photon::fs::IFile *GzIndexTest::gzindex = nullptr;
const char *GzIndexTest::fn_defile = "/fdata";
const char *GzIndexTest::fn_gzdata = "/fdata.gz";
const char *GzIndexTest::fn_gzindex = "/findex";

char uds_path[] = "/tmp/gzstream_test/stream_conv.sock";

int download(const std::string &url, const std::string &out) {
    // if (::access(out.c_str(), 0) == 0)
    //     return 0;
    auto base = std::string(basename(url.c_str()));
    // download file
    std::string cmd = "curl -sL -o " + out + " " + url;
    LOG_INFO(VALUE(cmd.c_str()));
    auto ret = system(cmd.c_str());
    if (ret != 0) {
        LOG_ERRNO_RETURN(0, -1, "download failed: `", url.c_str());
    }
    return 0;
}

void handler(photon::net::ISocketStream *sock) {
    ASSERT_NE(nullptr, sock);
    LOG_DEBUG("Accepted");
    photon::thread_yield();
    char recv[65536];
    size_t count = 0;
    auto dst = photon::fs::open_localfile_adaptor("/tmp/dest", O_TRUNC | O_CREAT | O_RDWR);
    DEFER(delete dst);
    // sock->read(recv, sizeof(ssize_t));
    // auto st_size = *(size_t*)recv;
    auto gzstream = open_gzstream_file(sock, 0);
    ASSERT_NE(gzstream, nullptr);
    DEFER(delete gzstream);
    while (true) {
        auto readn = gzstream->read(recv, 65536);
        if (readn <= 0)
            break;

        count += readn;
        dst->write(recv, readn);
    }
    auto fn_idx = gzstream->save_index();
    LOG_INFO("RECV `, fn_idx: `", count, fn_idx.c_str());


    // ASSERT_STREQ(sha256sum("/tmp/dest").c_str(), "sha256:562688d70dcd1596556e7c671c1266f6e9c22b4f4fb8344efa8bed88fc2bac7b");
    // ASSERT_STREQ(sha256sum(fn_idx.c_str()).c_str(), "sha256:af3ffd4965d83f3d235c48ce75e16a1f2edf12d0e5d82816d7066a8485aade82");

}

void uds_server() {
    auto sock = photon::net::new_uds_server(true);
    DEFER({ delete (sock); });
    ASSERT_EQ(0, sock->bind(uds_path));
    ASSERT_EQ(0, sock->listen(100));
    char path[PATH_MAX];
    ASSERT_EQ(0, sock->getsockname(path, PATH_MAX));
    EXPECT_EQ(0, strcmp(path, uds_path));
    LOG_INFO("uds server listening `", path);
    handler(sock->accept());
    photon::thread_yield_to(nullptr);
}

void uds_client(photon::fs::IFile *file) {

    photon::thread_yield_to(nullptr);
    auto cli = photon::net::new_uds_client();
    DEFER({ delete cli; });
    LOG_DEBUG("Connecting");
    auto sock = cli->connect(uds_path);
    DEFER(delete sock);
    LOG_DEBUG(VALUE(sock), VALUE(errno));
    char path[PATH_MAX];
    sock->getpeername(path, PATH_MAX);
    EXPECT_EQ(0, strcmp(path, uds_path));
    struct stat st;
    file->fstat(&st);
    LOG_INFO("Connected `, start send file data(size: `)", path, st.st_size);
    // sock->write(&st.st_size, sizeof(st.st_size));
    char buff[65536];
    auto count = 0;
    while (true) {
        auto readn = file->read(buff, 65536);
        ASSERT_NE(readn, -1);
        auto ret = sock->write(buff, readn);
        count += readn;
        ASSERT_EQ(ret, readn);
        if (readn != 65536)
            break;
    }
    LOG_INFO("SEND: `", count);

    return;
}

TEST_F(GzIndexTest, stream) {
    std::string workdir = "/tmp/gzstream_test/";
    mkdir(workdir.c_str(), 0755);
    auto lfs = photon::fs::new_localfs_adaptor(workdir.c_str());
    LOG_INFO("start streamFile test");
    std::vector<std::string> filelist = {
    //    "https://dadi-shared.oss-cn-beijing.aliyuncs.com/cri-containerd-cni-1.5.2-linux-amd64.tar.gz",
        "https://github.com/containerd/containerd/releases/download/v1.5.17/cri-containerd-cni-1.5.17-linux-amd64.tar.gz",
        "https://github.com/containerd/containerd/releases/download/v1.4.4/containerd-1.4.4-linux-amd64.tar.gz",
    //    "https://dadi-shared.oss-cn-beijing.aliyuncs.com/containerd-1.4.4-linux-amd64.tar.gz",
    //    "https://dadi-shared.oss-cn-beijing.aliyuncs.com/go1.13.linux-amd64.tar.gz",
        "https://go.dev/dl/go1.17.6.linux-amd64.tar.gz",
        // "https://dadi-shared.oss-cn-beijing.aliyuncs.com/go1.17.6.linux-amd64.tar.gz"
    };
    std::vector<std::string> tar_sha256sum = {
        "sha256:02adc5074f59777d2ca74c8a0291659f69291865184c987d9c10e58f58b162c2",
        // "sha256:05e8b01c1ddb6ba4f8c84e7dbc76529bdc09861f9ce17c213a49e8c334f184ed",
        "sha256:0ccf983abf0b0fb64cc969079982bc34761ce22d7a3236a40d49d840d150e09a",
        // "sha256:1041ec4e2f40156e0731be175388be4c67aeceb44829f988df213e9fd5f26dc9",
        "sha256:562688d70dcd1596556e7c671c1266f6e9c22b4f4fb8344efa8bed88fc2bac7b"
    };
    int i = 0;
    for (auto test_tgz : filelist) {
        auto jh1 = photon::thread_enable_join(photon::thread_create11(uds_server));
        std::string fn_test_tgz = basename(test_tgz.c_str());
        ASSERT_EQ(
            0, download(test_tgz, (workdir + fn_test_tgz).c_str()));
        auto file = lfs->open(fn_test_tgz.c_str(), O_RDONLY);
        std::string tar_sha256="", idx_sha256="";
        uds_client(file);
        photon::thread_join(jh1);
        auto dst_sha256 = sha256sum("/tmp/dest");
        ASSERT_STREQ(dst_sha256.c_str(), tar_sha256sum[i++].c_str());
        lfs->unlink(fn_test_tgz.c_str());
    }
    remove(uds_path);

}

TEST_F(GzIndexTest, pread) {
    std::vector<PreadTestCase> t{
        {0, 1, 1},
        {0, 10, 10},
        {1000000, 1000000, 1000000},
        {2000000, 1500000, 1500000},
        {(off_t)vsize - 10, 10, 10},
        {(off_t)vsize - 1, 1, 1},
    };
    group_test_pread(t);
}

TEST_F(GzIndexTest, pread_oob) {
    std::vector<PreadTestCase> t{
        {-1, 0, -1},
        {-1, 2, -1},
        {-1, 10000, -1},
        {-9999, 10000, -1},
        {(off_t)vsize, 1, 0},
        {(off_t)vsize - 1, 2, 1},
        {(off_t)vsize - 400, 1000, 400},
        {(off_t)vsize + 1, 1, 0},
        {(off_t)vsize + 10000, 10000, 0},
    };
    group_test_pread(t);
}

TEST_F(GzIndexTest, pread_rand) {
    std::vector<PreadTestCase> t;
    size_t n = 10000;
    for (size_t i = 0; i < n; i++) {
        size_t x = rand() % vsize;
        size_t y = rand() % vsize;
        if (x > y)
            std::swap(x, y);
        t.push_back({(off_t)x, y - x, (ssize_t)(y - x)});
    }
    group_test_pread(t);
}

TEST_F(GzIndexTest, fstat) {
    size_t data_size = vsize;
    struct stat st;
    EXPECT_EQ(gzfile->fstat(&st), 0);
    EXPECT_EQ(static_cast<size_t>(st.st_size), data_size);
    EXPECT_EQ(defile->fstat(&st), 0);
    EXPECT_EQ(static_cast<size_t>(st.st_size), data_size);
}

class GzCacheTest : public ::testing::Test {
protected:
    static photon::fs::IFile *defile;
    static photon::fs::IFile *gzfile;
    static const size_t vsize = 10 << 20;

    virtual void SetUp() override {
    }
    virtual void TearDown() override {
    }

    static void SetUpTestSuite() {
        std::string cmd = std::string("rm -r /tmp/gzip_*");
        system(cmd.c_str());
        cmd = std::string("mkdir -p /tmp/gzip_src");
        system(cmd.c_str());
        cmd = std::string("mkdir -p /tmp/gzip_cache_compress");
        system(cmd.c_str());
        cmd = std::string("mkdir -p /tmp/gzip_cache_decompress");
        system(cmd.c_str());
        lfs = photon::fs::new_localfs_adaptor("/tmp/gzip_src");
        if (buildDataFile() != 0) {
            LOG_ERROR("failed to build ` and `", fn_defile, fn_gzdata);
            exit(-1);
        }
        if (buildIndexFile() != 0) {
            LOG_ERROR("failed to build gz index: `", fn_gzindex);
            exit(-1);
        }

        auto mediafs = photon::fs::new_localfs_adaptor("/tmp/gzip_cache_compress");
        lfs = FileSystem::new_full_file_cached_fs(
                lfs, mediafs, 1024 * 1024, 1, 10000000,
                (uint64_t)1048576 * 4096, nullptr, 0, nullptr);
        delete gzdata;
        gzdata = lfs->open(fn_gzdata, O_RDONLY, 0644);
        if (gzdata == nullptr) {
            LOG_ERROR("gzdata create failed");
            exit(-1);
        }
        gzfile = new_gzfile(gzdata, gzindex);
        if (gzfile == nullptr) {
            LOG_ERROR("gzfile create failed");
            exit(-1);
        }


        mediafs = photon::fs::new_localfs_adaptor("/tmp/gzip_cache_decompress");
        cfs = Cache::new_gzip_cached_fs(mediafs, 1024 * 1024, 4, 10000000, (uint64_t)1048576 * 4096, nullptr);
        gzfile = cfs->open_cached_gzip_file(gzfile, fn_defile);
        if (gzfile == nullptr) {
            LOG_ERROR("failed create new cached gzip file");
            exit(-1);
        }
    }

    static void TearDownTestSuite() {
        delete gzdata;
        delete gzindex;
        delete defile;
        delete gzfile;

        lfs->unlink(fn_defile);
        lfs->unlink(fn_gzdata);
        lfs->unlink(fn_gzindex);

        delete lfs;
        delete cfs;
    }

    void test_pread(PreadTestCase t) {
        char *buf1 = new char[t.count];
        char *buf2 = new char[t.count];
        DEFER(delete[] buf1);
        DEFER(delete[] buf2);
        ssize_t ret1 = defile->pread(buf1, t.count, t.offset);
        ssize_t ret2 = gzfile->pread(buf2, t.count, t.offset);
        EXPECT_EQ(ret1, t.ret);
        EXPECT_EQ(ret2, t.ret);
        if (t.ret > 0) {
            EXPECT_EQ(strncmp(buf1, buf2, t.ret), 0);
        }
        LOG_DEBUG("pread testcase: { offset: `, count: `, ret: ` }", t.offset, t.count, t.ret);
    }

    void group_test_pread(std::vector<PreadTestCase> &t) {
        size_t testcases = t.size();
        LOG_INFO("Testing pread, ` sets of test cases ...", testcases);
        for (size_t i = 0; i < testcases; i++) {
            test_pread(t[i]);
        }
    }

private:
    static photon::fs::IFileSystem *lfs;
    static Cache::GzipCachedFs *cfs;
    static photon::fs::IFile *gzdata;
    static photon::fs::IFile *gzindex;

    static const char *fn_defile;
    static const char *fn_gzdata;
    static const char *fn_gzindex;

    static int buildDataFile() {
        // uncompressed data
        unsigned char *buf = new unsigned char[vsize];
        DEFER(delete[] buf);
        for (size_t i = 0; i < vsize; i++) {
            auto j = rand() % 256;
            buf[i] = j;
        }
        defile = lfs->open(fn_defile, O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (defile == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "failed to create `", fn_defile);
        }
        if (defile->pwrite(buf, vsize, 0) != vsize) {
            LOG_ERRNO_RETURN(0, -1, "failed to pwrite `", fn_defile);
        }
        // gzip data
        size_t gzlen = compressBound(vsize) + 4096;
        unsigned char *gzbuf = new unsigned char[gzlen];
        DEFER(delete[] gzbuf);
        if (gzip_compress(buf, vsize, gzbuf, gzlen) != 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to gzip_compress(...)");
        }
        gzdata = lfs->open(fn_gzdata, O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (gzdata == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "failed to create `", fn_gzdata);
        }
        if (gzdata->pwrite(gzbuf, gzlen, 0) != (ssize_t)gzlen) {
            LOG_ERRNO_RETURN(0, -1, "failed to pwrite `", fn_gzdata);
        }
        return 0;
    }

    static int buildIndexFile() {
        std::string fn_gzindex_path = std::string("/tmp/gzip_src/") + fn_gzindex;
        if (create_gz_index(gzdata, fn_gzindex_path.c_str()) != 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to create gz index: `", fn_gzindex);
        }
        gzindex = lfs->open(fn_gzindex, O_RDONLY, 0444);
        if (gzindex == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "failed to open gz index: `", fn_gzindex);
        }
        return 0;
    }

    static int gzip_compress(unsigned char *in, size_t in_len, unsigned char *out,
                             size_t &out_len) {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        int ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8,
                               Z_DEFAULT_STRATEGY);
        if (ret != Z_OK) {
            LOG_ERRNO_RETURN(0, -1, "failed to deflateInit2(...)");
        }
        strm.next_in = in;
        strm.avail_in = in_len;
        strm.next_out = out;
        strm.avail_out = out_len;
        ret = deflate(&strm, Z_FINISH);
        if (ret != Z_STREAM_END) {
            LOG_ERRNO_RETURN(0, -1, "ret of deflate(Z_FINISH) != Z_STREAM_END, ret:`", ret);
        }
        out_len -= strm.avail_out;
        LOG_INFO("uncompressed len: `, gzip len: `", in_len, out_len);
        return 0;
    }
};

photon::fs::IFileSystem *GzCacheTest::lfs = nullptr;
Cache::GzipCachedFs *GzCacheTest::cfs = nullptr;
photon::fs::IFile *GzCacheTest::defile = nullptr;
photon::fs::IFile *GzCacheTest::gzfile = nullptr;
photon::fs::IFile *GzCacheTest::gzdata = nullptr;
photon::fs::IFile *GzCacheTest::gzindex = nullptr;
const char *GzCacheTest::fn_defile = "/fdata";
const char *GzCacheTest::fn_gzdata = "/fdata.gz";
const char *GzCacheTest::fn_gzindex = "/findex";

bool check_in_interval(int val, int l, int r) {
    return l <= val && val < r;
}

TEST_F(GzCacheTest, cache_store) {
    std::vector<PreadTestCase> t{
        {0, 1, 1},
        {5 << 20, 1, 1},
        {(off_t)vsize - 1, 1, 1},
    };
    group_test_pread(t);

    unsigned char *cbuf1 = new unsigned char[vsize];
    unsigned char *cbuf2 = new unsigned char[vsize];
    DEFER(delete[] cbuf1);
    DEFER(delete[] cbuf2);
    auto fp1 = fopen("/tmp/gzip_src/fdata", "r");
    auto fp2 = fopen("/tmp/gzip_cache_decompress/fdata", "r");
    DEFER(fclose(fp1));
    DEFER(fclose(fp2));
    fread(cbuf1, 1, vsize, fp1);
    fread(cbuf2, 1, vsize, fp2);
    // refill_size is 1MB
    for (size_t i = 0; i < vsize; i++) {
        if (check_in_interval(i, 0, 1 << 20) ||
            check_in_interval(i, vsize - (1 << 20), vsize) ||
            check_in_interval(i, 5 << 20, 6 << 20)) {
            EXPECT_EQ(cbuf1[i], cbuf2[i]);
        } else {
            EXPECT_EQ((int)cbuf2[i], 0);
        }
    }
}

TEST_F(GzCacheTest, pread) {
    std::vector<PreadTestCase> t{
        {0, 1, 1},
        {0, 10, 10},
        {1000000, 1000000, 1000000},
        {2000000, 1500000, 1500000},
        {(off_t)vsize - 10, 10, 10},
        {(off_t)vsize - 1, 1, 1},
    };
    group_test_pread(t);
}

TEST_F(GzCacheTest, pread_rand) {
    std::vector<PreadTestCase> t;
    size_t n = 10000;
    for (size_t i = 0; i < n; i++) {
        size_t x = rand() % vsize;
        size_t y = rand() % vsize;
        if (x > y)
            std::swap(x, y);
        t.push_back({(off_t)x, y - x, (ssize_t)(y - x)});
    }
    group_test_pread(t);
}

TEST_F(GzCacheTest, pread_oob) {
    std::vector<PreadTestCase> t{
        {-1, 0, -1},
        {-1, 2, -1},
        {-1, 10000, -1},
        {-9999, 10000, -1},
        {(off_t)vsize, 1, 0},
        {(off_t)vsize - 1, 2, 1},
        {(off_t)vsize - 400, 1000, 400},
        {(off_t)vsize + 1, 1, 0},
        {(off_t)vsize + 10000, 10000, 0},
    };
    group_test_pread(t);
}

TEST_F(GzCacheTest, pread_little) {
    std::vector<PreadTestCase> t;
    size_t n = 100000;
    for (size_t i = 0; i < n; i++) {
        size_t x = rand() % vsize;
        size_t y = x + rand() % 4096;
        if (y >= vsize)
            y = vsize - 1;
        t.push_back({(off_t)x, y - x, (ssize_t)(y - x)});
    }
    group_test_pread(t);
}

TEST_F(GzCacheTest, fstat) {
    size_t data_size = vsize;
    struct stat st;
    EXPECT_EQ(gzfile->fstat(&st), 0);
    EXPECT_EQ(static_cast<size_t>(st.st_size), data_size);
    EXPECT_EQ(defile->fstat(&st), 0);
    EXPECT_EQ(static_cast<size_t>(st.st_size), data_size);
}

int main(int argc, char **argv) {
    auto seed = 154574045;
    std::cerr << "seed = " << seed << std::endl;
    srand(seed);
    set_log_output_level(1);

    ::testing::InitGoogleTest(&argc, argv);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    auto ret = RUN_ALL_TESTS();
    photon::fini();
    if (ret)
        LOG_ERROR_RETURN(0, ret, VALUE(ret));
}
