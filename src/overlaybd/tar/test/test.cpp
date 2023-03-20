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
#include <photon/photon.h>
#include <photon/fs/localfs.h>
#include <photon/fs/subfs.h>
#include <photon/common/alog.h>
#include "../libtar.h"
#include "../tar_file.cpp"

#define FILE_SIZE (2 * 1024 * 1024)
class TarTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        fs = photon::fs::new_localfs_adaptor();
        std::string base = "/tmp/tar_test";
        ASSERT_NE(nullptr, fs);
        if (fs->access(base.c_str(), 0) != 0) {
            auto ret = fs->mkdir(base.c_str(), 0755);
            ASSERT_EQ(0, ret);
        }
        // download file
        std::string cmd = "wget -q -O - https://github.com/containerd/overlaybd/archive/refs/tags/latest.tar.gz | gzip -d -c >" +
                          base + "/latest.tar";
        LOG_INFO(VALUE(cmd.c_str()));
        auto ret = system(cmd.c_str());
        ASSERT_EQ(0, ret);
        fs = photon::fs::new_subfs(fs, base.c_str(), true);
        ASSERT_NE(nullptr, fs);
    }
    static void TearDownTestSuite() {
        if (fs)
            delete fs;
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

    static photon::fs::IFileSystem *fs;
};
photon::fs::IFileSystem *TarTest::fs = nullptr;

TEST_F(TarTest, untar) {
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