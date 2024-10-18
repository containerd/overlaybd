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

#include <zlib.h>
#include <gtest/gtest.h>
#include <fcntl.h>
#include <photon/photon.h>
#include <photon/fs/localfs.h>
#include <photon/fs/subfs.h>
#include <photon/fs/extfs/extfs.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <vector>
#include "../../../gzindex/gzfile.h"
#include "../../../lsmt/file.h"
#include "../liberofs.h"
#include "../../tar_file.cpp"
#include "../../../gzip/gz.h"
#include "../../../../tools/sha256file.h"


#define FILE_SIZE (2 * 1024 * 1024)
#define IMAGE_SIZE 512UL<<20
class ErofsTest : public ::testing::Test {
protected:
    virtual void SetUp() override{
        fs = photon::fs::new_localfs_adaptor();

        ASSERT_NE(nullptr, fs);
        if (fs->access(workdir.c_str(), 0) != 0) {
            auto ret = fs->mkdir(workdir.c_str(), 0755);
            ASSERT_EQ(0, ret);
        }

        fs = photon::fs::new_subfs(fs, workdir.c_str(), true);
        ASSERT_NE(nullptr, fs);
    }
    virtual void TearDown() override{
        for (auto fn : filelist){
            fs->unlink(fn.c_str());
        }
        if (fs)
            delete fs;
    }

    int download(const std::string &url, std::string out = "") {
        if (out == "") {
            out = workdir + "/" + std::string(basename(url.c_str()));
        }
        if (fs->access(out.c_str(), 0) == 0)
            return 0;
        // download file
        std::string cmd = "curl -s -o " + out + " " + url;
        LOG_INFO(VALUE(cmd.c_str()));
        auto ret = system(cmd.c_str());
        if (ret != 0) {
            LOG_ERRNO_RETURN(0, -1, "download failed: `", url.c_str());
        }
        return 0;
    }

    int download_decomp(const std::string &url) {
        // download file
        std::string cmd = "wget -q -O - " + url +" | gzip -d -c >" +
                          workdir + "/test.tar";
        LOG_INFO(VALUE(cmd.c_str()));
        auto ret = system(cmd.c_str());
        if (ret != 0) {
            LOG_ERRNO_RETURN(0, -1, "download failed: `", url.c_str());
        }
        return 0;
    }

    int inflate(unsigned char *data, unsigned int size) {
        unsigned char out[65536];
        z_stream strm;
        int ret;
	/* allocate inflate state */
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.avail_in = 0;
        strm.next_in = Z_NULL;
        ret = inflateInit2(&strm, 31);
        if (ret != Z_OK)
           return ret;
        DEFER((void)inflateEnd(&strm));
        strm.avail_in = size;
        strm.next_in = data;
        int fd = open(std::string(workdir + "/test.tar").c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0644);
        if (fd < 0)
	   return fd;
	DEFER(close(fd));
        do {
           strm.avail_out = sizeof(out);
           strm.next_out = out;
           ret = ::inflate(&strm, Z_NO_FLUSH);
           switch (ret) {
            case Z_NEED_DICT:
            case Z_DATA_ERROR:
            case Z_MEM_ERROR:
                return -1;
            }
            int have = sizeof(out) - strm.avail_out;
            if (write(fd, out, have) != have) {
                return -1;
            }
        } while (strm.avail_out == 0);
        return 0;
    }
    int write_file(photon::fs::IFile *file) {
        std::string bb = "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz01";
        ssize_t size = 0;
        ssize_t ret;
        struct stat st;
        LOG_INFO(VALUE(bb.size()));
        while (size < FILE_SIZE) {
            ret = file->write(bb.data(), bb.size());
            EXPECT_EQ(bb.size(), ret);
            ret = file->fstat(&st);
            EXPECT_EQ(0, ret);
            ret = file->lseek(0, SEEK_CUR);
            EXPECT_EQ(st.st_size, ret);
            size += bb.size();
        }
        LOG_INFO("write ` byte", size);
        EXPECT_EQ(FILE_SIZE, size);
        return 0;
    }

    IFile *createDevice(const char *fn, IFile *target_file, size_t virtual_size = IMAGE_SIZE){
        auto fn_idx = std::string(fn)+".idx";
        auto fn_meta = std::string(fn)+".meta";
        DEFER({
            filelist.push_back(fn_idx);
            filelist.push_back(fn_meta);
        });
        auto fmeta = fs->open(fn_idx.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
        auto findex = fs->open(fn_meta.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
        LSMT::WarpFileArgs args(findex, fmeta, target_file);
        args.virtual_size = virtual_size;
        return create_warpfile(args, false);
    }

    int do_verify(IFile *verify, IFile *test, off_t offset = 0, ssize_t count = -1) {

        if (count == -1) {
            count = verify->lseek(0, SEEK_END);
            auto len = test->lseek(0, SEEK_END);
            if (count != len) {
                LOG_ERROR("check logical length failed");
                return -1;
            }
        }
        LOG_INFO("start verify, virtual size: `", count);

        ssize_t LEN = 1UL<<20;
        char vbuf[1UL<<20], tbuf[1UL<<20];
        // set_log_output_level(0);
        for (off_t i = 0; i < count; i+=LEN) {
            LOG_DEBUG("`", i);
            auto ret_v = verify->pread(vbuf, LEN, i);
            auto ret_t = test->pread(tbuf, LEN, i);
            if (ret_v == -1 || ret_t == -1) {
                LOG_ERROR_RETURN(0, -1, "pread(`,`) failed. (ret_v: `, ret_t: `)",
                    i, LEN, ret_v, ret_v);
            }
            if (ret_v != ret_t) {
                LOG_ERROR_RETURN(0, -1, "compare pread(`,`) return code failed. ret:` / `(expected)",
                    i, LEN, ret_t, ret_v);
            }
            if (memcmp(vbuf, tbuf, ret_v)!= 0){
                LOG_ERROR_RETURN(0, -1, "compare pread(`,`) buffer failed.", i, LEN);
            }
        }
        return 0;
    }

    std::string workdir = "/tmp/tar_test";
    photon::fs::IFileSystem *fs;
    std::vector<std::string> filelist;
};

TEST_F(ErofsTest, tar_meta) {
    unsigned char tar_zipped[249] = {
        0x1f, 0x8b, 0x08, 0x08, 0x7d, 0x06, 0x12, 0x67, 0x00, 0x03, 0x74,
        0x65, 0x73, 0x74, 0x2e, 0x74, 0x61, 0x72, 0x00, 0xed, 0xd7, 0x31,
        0x0e, 0xc2, 0x30, 0x0c, 0x85, 0xe1, 0xce, 0x9c, 0xa2, 0x47, 0x48,
        0x52, 0x27, 0x86, 0xe3, 0x14, 0xc4, 0x05, 0x68, 0xb8, 0x3f, 0x35,
        0xe9, 0xc0, 0xea, 0xc1, 0x2e, 0x92, 0xdf, 0xbf, 0x54, 0xea, 0xf2,
        0xb2, 0x7c, 0x52, 0xd2, 0x9f, 0x5b, 0x9f, 0x8c, 0x4b, 0x7b, 0x8d,
        0x48, 0xbe, 0x99, 0x6b, 0xfa, 0xfd, 0x1e, 0xd1, 0x94, 0x89, 0x13,
        0x51, 0xca, 0x39, 0xcb, 0xff, 0xd6, 0x5a, 0x99, 0xe6, 0x64, 0x7d,
        0x30, 0xe9, 0xbd, 0xf5, 0xf5, 0x35, 0xcf, 0x1e, 0x53, 0xff, 0xd8,
        0x7a, 0x7f, 0x5c, 0xce, 0x3e, 0x03, 0x3a, 0xaf, 0xbe, 0xfb, 0x2f,
        0xc6, 0x1b, 0x0a, 0xff, 0x85, 0x4b, 0x16, 0xff, 0x5c, 0x2b, 0xfc,
        0x7b, 0x04, 0xff, 0xb1, 0x13, 0xff, 0x8b, 0xf1, 0x86, 0xce, 0x3f,
        0x7f, 0xfd, 0x37, 0x82, 0x7f, 0x8f, 0xe0, 0x3f, 0x76, 0xe2, 0x9f,
        0x8c, 0x37, 0x54, 0xfe, 0x17, 0x1a, 0xfe, 0x17, 0xf8, 0xf7, 0x08,
        0xfe, 0x63, 0x27, 0xfe, 0xab, 0xf1, 0x86, 0xca, 0x3f, 0x8f, 0xfb,
        0x7f, 0xc3, 0xfd, 0xdf, 0x25, 0xf8, 0x8f, 0x9d, 0xf8, 0x6f, 0xc6,
        0x1b, 0x0a, 0xff, 0x4b, 0x2a, 0x65, 0xbc, 0xff, 0x71, 0xff, 0x77,
        0x09, 0xfe, 0x63, 0x27, 0xfe, 0xd9, 0x78, 0x43, 0xe7, 0x7f, 0xdc,
        0xff, 0x2b, 0xc3, 0xbf, 0x47, 0xf0, 0x1f, 0x3b, 0xf1, 0x7f, 0x35,
        0xde, 0x50, 0xf9, 0xdf, 0x1f, 0xfe, 0x78, 0xff, 0xfb, 0x05, 0xff,
        0xb1, 0x13, 0xff, 0x37, 0xe3, 0x0d, 0x8d, 0xff, 0x9c, 0x0f, 0xff,
        0x09, 0xfe, 0x3d, 0x82, 0x7f, 0x84, 0x10, 0x8a, 0xd9, 0x07, 0xbf,
        0x49, 0x1c, 0x0f, 0x00, 0x28, 0x00, 0x00
    };
    // set_log_output_level(0);
    ASSERT_EQ(0, inflate(tar_zipped, sizeof(tar_zipped)));

    auto src_file = fs->open("test.tar", O_RDONLY, 0666);
    ASSERT_NE(nullptr, src_file);
    DEFER(delete src_file);
    auto verify_dev = createDevice("verify", src_file);
    auto tar = new LibErofs(verify_dev, 4096, false);
    ASSERT_EQ(0, tar->extract_tar(src_file, true, true));
    delete tar;

    src_file->lseek(0, 0);

    auto tar_idx = fs->open("test.tar.meta", O_TRUNC | O_CREAT | O_RDWR, 0644);
    auto imgfile = createDevice("mock", src_file);
    DEFER(delete imgfile;);
    auto tar2 = new UnTar(src_file, nullptr, 0, 4096, nullptr, true);
    auto obj_count = tar2->dump_tar_headers(tar_idx);
    EXPECT_NE(-1, obj_count);
    LOG_INFO("objects count: `", obj_count);
    tar_idx->lseek(0,0);
    delete tar2;

    auto tar3 = new LibErofs(imgfile, 4096, true);
    ASSERT_EQ(0, tar3->extract_tar(tar_idx, true, true));
    delete tar3;
    EXPECT_EQ(0, do_verify(verify_dev, imgfile));
    delete tar_idx;
}

int main(int argc, char **argv) {

    ::testing::InitGoogleTest(&argc, argv);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    set_log_output_level(1);

    auto ret = RUN_ALL_TESTS();
    (void)ret;

    return 0;
}
