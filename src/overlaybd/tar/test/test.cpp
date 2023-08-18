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

#include <cstring>
#include <gtest/gtest.h>
#include <fcntl.h>
#include <photon/photon.h>
#include <photon/fs/localfs.h>
#include <photon/fs/subfs.h>
#include <photon/common/alog.h>
#include <vector>
#include "../../extfs/extfs.h"
#include "../../lsmt/file.h"
#include "../libtar.h"
#include "../tar_file.cpp"


#define FILE_SIZE (2 * 1024 * 1024)
#define IMAGE_SIZE 512UL<<20
class TarTest : public ::testing::Test {
protected:
    virtual void SetUp() override{
        fs = photon::fs::new_localfs_adaptor();

        ASSERT_NE(nullptr, fs);
        if (fs->access(base.c_str(), 0) != 0) {
            auto ret = fs->mkdir(base.c_str(), 0755);
            ASSERT_EQ(0, ret);
        }

        fs = photon::fs::new_subfs(fs, base.c_str(), true);
        ASSERT_NE(nullptr, fs);
    }
    virtual void TearDown() override{
        for (auto fn : filelist){
            fs->unlink(fn.c_str());
        }
        if (fs)
            delete fs;
    }

    int download(const std::string &url) {
        // download file
        std::string cmd = "wget -q -O - " + url +" | gzip -d -c >" +
                          base + "/latest.tar";
        LOG_INFO(VALUE(cmd.c_str()));
        auto ret = system(cmd.c_str());
        if (ret != 0) {
            LOG_ERRNO_RETURN(0, -1, "download failed: `", url.c_str());
        }
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
        for (off_t i = 0; i < count; i+=LEN) {
            // LOG_INFO("`", i);
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

    std::string base = "/tmp/tar_test";
    photon::fs::IFileSystem *fs;
    std::vector<std::string> filelist;
};
// photon::fs::IFileSystem *TarTest::fs = nullptr;

TEST_F(TarTest, untar) {
    ASSERT_EQ(0, download("https://github.com/containerd/overlaybd/archive/refs/tags/latest.tar.gz"));
    auto tarf = fs->open("latest.tar", O_RDONLY, 0666);
    ASSERT_NE(nullptr, tarf);
    DEFER(delete tarf);
    if (fs->access("rootfs", 0) != 0) {
        fs->mkdir("rootfs", 0755);
    }
    auto target = photon::fs::new_subfs(fs, "rootfs", false);
    ASSERT_NE(nullptr, target);
    DEFER(delete target);
    auto tar = new UnTar(tarf, target, 0);
    auto ret = tar->extract_all();
    EXPECT_EQ(0, ret);
    delete tar;
}

TEST_F(TarTest, tar_meta) {
    // set_log_output_level(0);
    ASSERT_EQ(0, download("https://dadi-shared.oss-cn-beijing.aliyuncs.com/go1.17.6.linux-amd64.tar.gz"));
    // ASSERT_EQ(0, download("https://github.com/containerd/overlaybd/archive/refs/tags/latest.tar.gz"));

    auto src_file = fs->open("latest.tar", O_RDONLY, 0666);
    ASSERT_NE(nullptr, src_file);
    DEFER(delete src_file);
    auto verify_dev = createDevice("verify", src_file);
    make_extfs(verify_dev);
    auto verify_ext4fs = new_extfs(verify_dev, false);
    auto verifyfs = new_subfs(verify_ext4fs, "/", true);
    auto fastoci_verify = new UnTar(src_file, verifyfs, 0, 4096, verify_dev, true);
    ASSERT_EQ(0, fastoci_verify->extract_all());
    verify_ext4fs->sync();
    delete fastoci_verify;
    delete verifyfs;

    src_file->lseek(0, 0);

    auto tar_idx = fs->open("latest.tar.meta", O_TRUNC | O_CREAT | O_RDWR, 0644);
    auto imgfile = createDevice("mock", src_file);
    DEFER(delete imgfile;);
    auto tar = new UnTar(src_file, nullptr, 0, 4096, nullptr, false);
    auto obj_count = tar->dump_tar_headers(tar_idx);
    EXPECT_NE(-1, obj_count);
    LOG_INFO("objects count: `", obj_count);
    tar_idx->lseek(0,0);

    make_extfs(imgfile);
    auto extfs = new_extfs(imgfile, false);
    auto target = new_subfs(extfs, "/", true);
    auto fastoci_mock = new UnTar(tar_idx, target, TAR_IGNORE_CRC, 4096, imgfile, true, true);
    auto ret = fastoci_mock->extract_all();
    delete fastoci_mock;
    delete target;

    ASSERT_EQ(0, ret);
    EXPECT_EQ(0, do_verify(verify_dev, imgfile));
    delete tar_idx;
    delete tar;

    // EXPECT_EQ(0, ret);
}

TEST_F(TarTest, tar_header_check) {
    auto fn = "data";
    auto tarfs = new_tar_fs_adaptor(fs);
    auto file = tarfs->open(fn, O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_NE(nullptr, file);

    struct stat st;
    auto ret = file->fstat(&st);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0, st.st_size);

    write_file(file);
    delete file;

    file = fs->open(fn, O_RDONLY);
    ASSERT_NE(nullptr, file);
    auto istar = is_tar_file(file);
    EXPECT_EQ(1, istar);
    auto tar_file = new_tar_file_adaptor(file);
    ASSERT_NE(nullptr, tar_file);
    DEFER(delete tar_file);
    ret = tar_file->fstat(&st);
    EXPECT_EQ(FILE_SIZE, st.st_size);

    char buf[16];
    ret = tar_file->pread(buf, 16, 0);
    EXPECT_EQ(16, ret);
    EXPECT_EQ(0, memcmp(buf, "abcdefghijklmnop", 16));
    ret = tar_file->pread(buf, 16, 16384);
    EXPECT_EQ(16, ret);
    EXPECT_EQ(0, memcmp(buf, "abcdefghijklmnop", 16));
    ret = tar_file->lseek(1, SEEK_SET);
    EXPECT_EQ(1, ret);
    ret = tar_file->read(buf, 16);
    EXPECT_EQ(16, ret);
    EXPECT_EQ(0, memcmp(buf, "bcdefghijklmnopq", 16));
    ret = tar_file->lseek(0, SEEK_CUR);
    EXPECT_EQ(17, ret);
    ret = tar_file->lseek(0, SEEK_END);
    EXPECT_EQ(FILE_SIZE, ret);
}

int main(int argc, char **argv) {

    ::testing::InitGoogleTest(&argc, argv);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    set_log_output_level(1);

    auto ret = RUN_ALL_TESTS();
    (void)ret;

    return 0;
}