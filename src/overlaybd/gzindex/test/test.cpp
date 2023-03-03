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
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <gtest/gtest.h>
#include <iostream>
#include <fcntl.h>
#include <zlib.h>
#include <vector>

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
        DEFER(delete []buf1);
        DEFER(delete []buf2);
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
        DEFER(delete []buf);
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
        DEFER(delete []gzbuf);
        if (gzip_compress(buf, vsize, gzbuf, gzlen) != 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to gzip_compress(...)");
        }
        gzdata = lfs->open(fn_gzdata, O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (gzdata == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "failed to create `", fn_gzdata);
        }
        if (gzdata->pwrite(gzbuf, gzlen, 0) != gzlen) {
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

    static int gzip_compress(unsigned char *in, size_t in_len, unsigned char *out, size_t &out_len) {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        int ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
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
const char *GzIndexTest::fn_defile = "fdata";
const char *GzIndexTest::fn_gzdata = "fdata.gz";
const char *GzIndexTest::fn_gzindex = "findex";

TEST_F(GzIndexTest, pread) {
    std::vector<PreadTestCase> t{
        {0, 1, 1},
        {0, 10, 10},
        {1000000, 1000000, 1000000},
        {2000000, 1500000, 1500000},
        {(off_t) vsize - 10, 10, 10},
        {(off_t) vsize - 1, 1, 1},
    };
    group_test_pread(t);
}

TEST_F(GzIndexTest, pread_oob) {
    std::vector<PreadTestCase> t{
        {-1, 0, -1},
        {-1, 2, -1},
        {-1, 10000, -1},
        {-9999, 10000, -1},
        {(off_t) vsize, 1, 0},
        {(off_t) vsize - 1, 2, 1},
        {(off_t) vsize - 400, 1000, 400},
        {(off_t) vsize + 1, 1, 0},
        {(off_t) vsize + 10000, 10000, 0},
    };
    group_test_pread(t);
}

TEST_F(GzIndexTest, pread_rand) {
    std::vector<PreadTestCase> t;
    size_t n = 10000;
    for (size_t i = 0; i < n; i++) {
        size_t x = rand() % vsize;
        size_t y = rand() % vsize;
        if (x > y) std::swap(x, y);
        t.push_back({(off_t) x , y - x, (ssize_t) (y - x)});
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

int main(int argc, char **argv) {
    auto seed = 154574045;
    std::cerr << "seed = " << seed << std::endl;
    srand(seed);
    set_log_output_level(1);

    ::testing::InitGoogleTest(&argc, argv);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER(photon::fini(););
    auto ret = RUN_ALL_TESTS();
    if (ret) LOG_ERROR_RETURN(0, ret, VALUE(ret));
}
