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
#include <photon/fs/extfs/extfs.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <vector>
#include "../../gzindex/gzfile.h"
#include "../../lsmt/file.h"
#include "../libtar.h"
#include "../tar_file.cpp"
#include "../../gzip/gz.h"
#include "../../../tools/sha256file.h"


#define FILE_SIZE (2 * 1024 * 1024)
#define IMAGE_SIZE 512UL<<20
class TarTest : public ::testing::Test {
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
                          workdir + "/latest.tar";
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
// photon::fs::IFileSystem *TarTest::fs = nullptr;

TEST_F(TarTest, untar) {
    ASSERT_EQ(0, download_decomp("https://github.com/containerd/overlaybd/archive/refs/tags/latest.tar.gz"));
    auto tarf = fs->open("latest.tar", O_RDONLY, 0666);
    ASSERT_NE(nullptr, tarf);
    DEFER(delete tarf);
    if (fs->access("rootfs", 0) != 0) {
        fs->mkdir("rootfs", 0755);
    }
    auto target = photon::fs::new_subfs(fs, "rootfs", false);
    ASSERT_NE(nullptr, target);
    DEFER(delete target);
    auto tar = new UnTar(tarf, target, TAR_CHECK_EUID);
    auto ret = tar->extract_all();
    EXPECT_EQ(0, ret);
    delete tar;
}

TEST_F(TarTest, tar_meta) {
    // set_log_output_level(0);
    ASSERT_EQ(0, download_decomp("https://dadi-shared.oss-cn-beijing.aliyuncs.com/go1.17.6.linux-amd64.tar.gz"));

    auto src_file = fs->open("latest.tar", O_RDONLY, 0666);
    ASSERT_NE(nullptr, src_file);
    DEFER(delete src_file);
    auto verify_dev = createDevice("verify", src_file);
    make_extfs(verify_dev);
    auto verify_ext4fs = new_extfs(verify_dev, false);
    auto verifyfs = new_subfs(verify_ext4fs, "/", true);
    auto turboOCI_verify = new UnTar(src_file, verifyfs, 0, 4096, verify_dev, true);
    ASSERT_EQ(0, turboOCI_verify->extract_all());
    verify_ext4fs->sync();
    delete turboOCI_verify;
    delete verifyfs;

    src_file->lseek(0, 0);

    auto tar_idx = fs->open("latest.tar.meta", O_TRUNC | O_CREAT | O_RDWR, 0644);
    auto imgfile = createDevice("mock", src_file);
    DEFER(delete imgfile;);
    auto tar = new UnTar(src_file, nullptr, 0, 4096, nullptr, true);
    auto obj_count = tar->dump_tar_headers(tar_idx);
    EXPECT_NE(-1, obj_count);
    LOG_INFO("objects count: `", obj_count);
    tar_idx->lseek(0,0);

    make_extfs(imgfile);
    auto extfs = new_extfs(imgfile, false);
    auto target = new_subfs(extfs, "/", true);
    auto turboOCI_mock = new UnTar(tar_idx, target, TAR_IGNORE_CRC, 4096, imgfile, true, true);
    auto ret = turboOCI_mock->extract_all();
    delete turboOCI_mock;
    delete target;

    ASSERT_EQ(0, ret);
    EXPECT_EQ(0, do_verify(verify_dev, imgfile));
    delete tar_idx;
    delete tar;

}

TEST_F(TarTest, stream) {
    set_log_output_level(1);
    std::string fn_test_tgz = "go1.17.6.linux-amd64.tar.gz";
    ASSERT_EQ(
        0, download("https://dadi-shared.oss-cn-beijing.aliyuncs.com/go1.17.6.linux-amd64.tar.gz",
                    ""));
    set_log_output_level(0);

    for (int i = 0; i < 3; i++) {
        auto src_file = fs->open(fn_test_tgz.c_str(), O_RDONLY, 0644);
        struct stat st;
        src_file->fstat(&st);
        auto streamfile = open_gzstream_file(src_file, 0);
        auto fn = ("/tmp/tar_test/" + fn_test_tgz);
        ASSERT_NE(nullptr, src_file);
        DEFER(delete src_file);

        auto turboOCI_stream = new UnTar(streamfile, nullptr, 0, 4096, nullptr, true);
        DEFER(delete turboOCI_stream);

        auto tar_idx =  fs->open("stream.tar.meta", O_TRUNC | O_CREAT | O_RDWR, 0644);
        DEFER(delete tar_idx);
        auto obj_count = turboOCI_stream->dump_tar_headers(tar_idx);
        EXPECT_NE(-1, obj_count);
        tar_idx->lseek(0, SEEK_SET);
        auto tar_meta_sha256 = new_sha256_file(tar_idx, false);
        DEFER(delete tar_meta_sha256);
        ASSERT_STREQ(tar_meta_sha256->sha256_checksum().c_str(), "sha256:c5aaa64a1b70964758e190b88b3e65528607b0002bffe42513bc65ac6e65f337");
        auto idx_fn = streamfile->save_index();
        // auto idx_fn = "/tmp/test.idx";

        // create_gz_index(src_file, idx_fn);
        auto idx_sha256 = sha256sum(idx_fn.c_str());
        delete streamfile;
        ASSERT_STREQ(idx_sha256.c_str(), "sha256:af3ffd4965d83f3d235c48ce75e16a1f2edf12d0e5d82816d7066a8485aade82");
    }
}

TEST_F(TarTest, gz_tarmeta_e2e) {
    // set_log_output_level(0);
    std::vector<std::string> filelist {
        "https://dadi-shared.oss-cn-beijing.aliyuncs.com/cri-containerd-cni-1.5.2-linux-amd64.tar.gz",
        "https://dadi-shared.oss-cn-beijing.aliyuncs.com/containerd-1.4.4-linux-amd64.tar.gz",
        "https://dadi-shared.oss-cn-beijing.aliyuncs.com/go1.17.6.linux-amd64.tar.gz"
    };
    for (auto file : filelist){
        ASSERT_EQ(0, download(file.c_str()));
        auto fn = std::string(basename(file.c_str()));
        auto gzip_file = fs->open(fn.c_str(), O_RDONLY, 0600);
        auto gzfile = open_gzfile_adaptor((workdir + "/" + fn).c_str());
        auto fn_idx = (workdir + "/" + fn + ".gz_idx");
        ASSERT_EQ(create_gz_index(gzip_file, fn_idx.c_str()), 0);
        auto gz_idx = fs->open((fn + ".gz_idx").c_str(), O_RDONLY, 0644);
        gzip_file->lseek(0, SEEK_SET);
        auto src_file = new_gzfile(gzip_file, gz_idx, true);
        ASSERT_NE(nullptr, src_file);
        auto verify_dev = createDevice((fn + ".verify").c_str(), src_file);
        make_extfs(verify_dev);
        auto verify_ext4fs = new_extfs(verify_dev, false);
        auto verifyfs = new_subfs(verify_ext4fs, "/", true);
        // gzfile->lseek(0, SEEK_SET);
        auto turboOCI_verify = new UnTar(gzfile, verifyfs, 0, 4096, verify_dev, true);
        ASSERT_EQ(0, turboOCI_verify->extract_all());
        verify_ext4fs->sync();

        // src_file->lseek(0, 0);
        auto tar_idx = fs->open((fn + ".tar.meta").c_str(), O_TRUNC | O_CREAT | O_RDWR, 0644);
        auto stream_src = fs->open(fn.c_str(), O_RDONLY, 0600);
        auto streamfile = open_gzstream_file(stream_src, 0);
        auto tar = new UnTar(streamfile, nullptr, 0, 4096, nullptr, true);
        auto obj_count = tar->dump_tar_headers(tar_idx);
        EXPECT_NE(-1, obj_count);
        LOG_INFO("objects count: `", obj_count);

        auto fn_test_idx = streamfile->save_index();
        LOG_INFO("gzip index of [`]: `", fn, fn_test_idx);
        auto test_gz_idx = open_localfile_adaptor(fn_test_idx.c_str(), O_RDONLY);
        ASSERT_NE(test_gz_idx, nullptr);
        auto test_gzfile = fs->open(fn.c_str(), O_RDONLY, 0600);
        ASSERT_NE(test_gzfile, nullptr);
        auto gz_target = new_gzfile(test_gzfile,test_gz_idx, true);
        auto imgfile = createDevice((fn + ".mock").c_str(), gz_target);

        tar_idx->lseek(0,0);

        make_extfs(imgfile);
        auto extfs = new_extfs(imgfile, false);
        auto target = new_subfs(extfs, "/", true);
        auto turboOCI_mock = new UnTar(tar_idx, target, TAR_IGNORE_CRC, 4096, imgfile, true, true);
        auto ret = turboOCI_mock->extract_all();
        extfs->sync();

        ASSERT_EQ(0, ret);
        EXPECT_EQ(0, do_verify(verify_dev, imgfile));

        delete turboOCI_mock;
        delete target;
        delete src_file;
        delete gzfile;
        delete turboOCI_verify;
        delete verifyfs;
        delete tar_idx;
        delete stream_src;
        delete streamfile;
        delete tar;

        delete verify_dev;
        delete imgfile;
    }

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

TEST(CleanNameTest, clean_name) {
    char name[256] = {0};
    char *cname;
    // 1. Reduce multiple slashes to a single slash.
    strcpy(name, "/tar_test///busybox");
    cname = clean_name(name);
    EXPECT_EQ(0, strcmp(cname, "/tar_test/busybox"));
    // 2. Eliminate . path name elements (the current directory).
    strcpy(name, "/tar_test/./busybox");
    cname = clean_name(name);
    EXPECT_EQ(0, strcmp(cname, "/tar_test/busybox"));
    // 3. Eliminate .. path name elements (the parent directory) and the non-. non-.., element that precedes them.
    strcpy(name, "/tar_test/bin/../busybox");
    cname = clean_name(name);
    EXPECT_EQ(0, strcmp(cname, "/tar_test/busybox"));
    strcpy(name, "/tar_test/bin/./../busybox");
    cname = clean_name(name);
    EXPECT_EQ(0, strcmp(cname, "/tar_test/busybox"));
    strcpy(name, "/tar_test/test/bin/./../../busybox");
    cname = clean_name(name);
    EXPECT_EQ(0, strcmp(cname, "/tar_test/busybox"));
    // 4. Eliminate .. elements that begin a rooted path, that is, replace /.. by / at the beginning of a path.
    strcpy(name, "/.././tar_test/./test/bin/../busybox");
    cname = clean_name(name);
    EXPECT_EQ(0, strcmp(cname, "/tar_test/test/busybox"));
    // 5. Leave intact .. elements that begin a non-rooted path.
    strcpy(name, ".././tar_test/./test/bin/../busybox");
    cname = clean_name(name);
    EXPECT_EQ(0, strcmp(cname, "../tar_test/test/busybox"));
    // If the result of this process is a null string, cleanname returns the string ".", representing the current directory.
    strcpy(name, "");
    cname = clean_name(name);
    EXPECT_EQ(0, strcmp(cname, "."));
    strcpy(name, "./");
    cname = clean_name(name);
    EXPECT_EQ(0, strcmp(cname, "."));
    // root is remained
    strcpy(name, "/");
    cname = clean_name(name);
    EXPECT_EQ(0, strcmp(cname, "/"));
    // tailing '/' is removed
    strcpy(name, "tar_test/");
    cname = clean_name(name);
    EXPECT_EQ(0, strcmp(cname, "tar_test"));
}

int main(int argc, char **argv) {

    ::testing::InitGoogleTest(&argc, argv);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    set_log_output_level(1);

    auto ret = RUN_ALL_TESTS();
    (void)ret;

    return 0;
}
