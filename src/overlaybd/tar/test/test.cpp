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
#include <photon/fs/path.h>
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

// A minimal tar writer, so that member names can be spelled exactly as needed.
// Real world layers come both with a leading slash ("/usr/lib") and without
// ("usr/lib"), and both spellings have to land on the same path.
class TarBuilder {
public:
    TarBuilder(photon::fs::IFileSystem *fs, const std::string &fn, const std::string &prefix)
        : m_prefix(prefix) {
        m_file = fs->open(fn.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    }
    ~TarBuilder() {
        if (m_file) {
            char zero[2 * T_BLOCKSIZE] = {0}; // end-of-archive marker
            m_file->write(zero, sizeof(zero));
            delete m_file;
        }
    }
    bool ok() { return m_file != nullptr; }
    // the entry most OCI layers start with, always stored verbatim as "./"
    void add_dot() { put("./", DIRTYPE, 0755, "", ""); }
    void add_dir(const std::string &name, mode_t mode = 0755) {
        put(m_prefix + name + "/", DIRTYPE, mode, "", "");
    }
    void add_file(const std::string &name, const std::string &content) {
        put(m_prefix + name, REGTYPE, 0644, "", content);
    }
    void add_symlink(const std::string &name, const std::string &target) {
        put(m_prefix + name, SYMTYPE, 0777, target, "");
    }
    // unlike a symlink target, a hardlink target is a path inside the target fs
    // and thus carries the same prefix as the member names
    void add_hardlink(const std::string &name, const std::string &target) {
        put(m_prefix + name, LNKTYPE, 0644, m_prefix + target, "");
    }

private:
    void put(const std::string &name, char typeflag, mode_t mode,
             const std::string &linkname, const std::string &content) {
        TarHeader h;
        memset(&h, 0, T_BLOCKSIZE);
        strncpy(h.name, name.c_str(), sizeof(h.name) - 1);
        strncpy(h.linkname, linkname.c_str(), sizeof(h.linkname) - 1);
        memcpy(h.magic, TMAGIC, TMAGLEN);
        memcpy(h.version, TVERSION, TVERSLEN);
        h.typeflag = typeflag;
        int_to_oct(mode, h.mode, (int)sizeof(h.mode));
        int_to_oct(0, h.uid, (int)sizeof(h.uid));
        int_to_oct(0, h.gid, (int)sizeof(h.gid));
        int_to_oct(content.size(), h.size, (int)sizeof(h.size));
        int_to_oct(0, h.mtime, (int)sizeof(h.mtime));

        memset(h.chksum, ' ', sizeof(h.chksum));
        auto raw = (unsigned char *)&h;
        unsigned int sum = 0;
        for (int i = 0; i < T_BLOCKSIZE; i++)
            sum += raw[i];
        int_to_oct(sum, h.chksum, (int)sizeof(h.chksum) - 1);

        m_file->write(raw, T_BLOCKSIZE);
        if (!content.empty()) {
            char block[T_BLOCKSIZE] = {0};
            size_t off = 0;
            while (off < content.size()) {
                auto n = std::min(content.size() - off, (size_t)T_BLOCKSIZE);
                memset(block, 0, T_BLOCKSIZE);
                memcpy(block, content.data() + off, n);
                m_file->write(block, T_BLOCKSIZE);
                off += n;
            }
        }
    }

    photon::fs::IFile *m_file = nullptr;
    std::string m_prefix;
};

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

    // Extracts three stacked layers whose members all carry `prefix` and checks
    // that the result does not depend on it. Layer 2 replaces a lower layer
    // directory with a symlink and layer 3 whites out lower layer entries, both
    // of which drive UnTar into remove_all() -> opendir(), i.e. the extfs
    // lookups that follow the last path component.
    void check_layer_names(const std::string &prefix) {
        auto tag = prefix.empty() ? std::string("unrooted") : std::string("rooted");
        auto imgfn = tag + ".img";
        filelist.push_back(imgfn);
        auto imgfile = fs->open(imgfn.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        ASSERT_NE(nullptr, imgfile);
        DEFER(delete imgfile);
        ASSERT_EQ(0, imgfile->ftruncate(IMAGE_SIZE));
        ASSERT_EQ(0, make_extfs(imgfile));
        // no subfs here, matching create_ext4fs()
        auto target = new_extfs(imgfile, false);
        ASSERT_NE(nullptr, target);
        DEFER(delete target);

        auto untar = [&](const std::string &fn) {
            auto tarf = fs->open(fn.c_str(), O_RDONLY, 0644);
            if (tarf == nullptr)
                return -1;
            DEFER(delete tarf);
            UnTar tar(tarf, target, 0, 4096);
            return tar.extract_all();
        };

        auto fn1 = tag + "_1.tar", fn2 = tag + "_2.tar", fn3 = tag + "_3.tar";
        auto fn4 = tag + "_4.tar", fn5 = tag + "_5.tar", fn6 = tag + "_6.tar";
        auto fn7 = tag + "_7.tar";
        filelist.push_back(fn1);
        filelist.push_back(fn2);
        filelist.push_back(fn3);
        filelist.push_back(fn4);
        filelist.push_back(fn5);
        filelist.push_back(fn6);
        filelist.push_back(fn7);

        {
            TarBuilder tb(fs, fn1, prefix);
            ASSERT_TRUE(tb.ok());
            if (!prefix.empty())
                tb.add_dot();
            tb.add_dir("usr");
            tb.add_dir("usr/lib");
            tb.add_dir("usr/lib/locale");
            tb.add_file("usr/lib/locale/locale-archive", "archive payload");
            tb.add_dir("usr/lib/locale/sub");
            tb.add_file("usr/lib/locale/sub/deep", "deep payload");
            tb.add_dir("usr/lib/gconv");
            tb.add_file("usr/lib/gconv/mod", "mod payload");
            tb.add_dir("usr/share");
            tb.add_file("usr/share/lower-only", "lower payload");
            tb.add_hardlink("usr/lib/hard", "usr/lib/locale/locale-archive");
            // a hardlink target is a path too, so it has to be cleaned and rooted
            // the very same way the member names are
            tb.add_hardlink("usr/lib/hard2", "usr/lib/../lib/locale/locale-archive");
            tb.add_symlink("usr/lib/rel", "../lib/locale");
            // names that only clean_name() can make sense of
            tb.add_file("usr/lib/./nested/../messy", "messy payload");
            tb.add_file("usr//lib///dslash", "dslash payload");
        }
        ASSERT_EQ(0, untar(fn1));

        struct stat st;
        ASSERT_EQ(0, target->lstat("/usr/lib/locale/locale-archive", &st));
        auto archive_ino = st.st_ino;
        EXPECT_EQ(3, st.st_nlink);
        ASSERT_EQ(0, target->lstat("/usr/lib/hard", &st));
        EXPECT_EQ(archive_ino, st.st_ino);
        ASSERT_EQ(0, target->lstat("/usr/lib/hard2", &st));
        EXPECT_EQ(archive_ino, st.st_ino);
        char buf[128] = {0};
        ASSERT_GT(target->readlink("/usr/lib/rel", buf, sizeof(buf)), 0);
        EXPECT_STREQ("../lib/locale", buf); // a symlink target stays verbatim
        EXPECT_EQ(0, target->lstat("/usr/lib/messy", &st));
        EXPECT_EQ(0, target->lstat("/usr/lib/dslash", &st));
        EXPECT_EQ(-1, target->lstat("/usr/lib/nested", &st));
        // stat() and opendir() resolve the whole path in one go
        ASSERT_EQ(0, target->stat("/usr/lib/locale", &st));
        auto dir = target->opendir("/usr/lib/locale");
        ASSERT_NE(nullptr, dir);
        target->closedir(dir);

        // layer 2: the locale directory becomes a symlink
        {
            TarBuilder tb(fs, fn2, prefix);
            ASSERT_TRUE(tb.ok());
            if (!prefix.empty())
                tb.add_dot();
            tb.add_dir("usr");
            tb.add_dir("usr/lib");
            tb.add_symlink("usr/lib/locale", "/usr/share/locale");
        }
        ASSERT_EQ(0, untar(fn2));
        ASSERT_EQ(0, target->lstat("/usr/lib/locale", &st));
        EXPECT_TRUE(S_ISLNK(st.st_mode));
        memset(buf, 0, sizeof(buf));
        ASSERT_GT(target->readlink("/usr/lib/locale", buf, sizeof(buf)), 0);
        EXPECT_STREQ("/usr/share/locale", buf);
        EXPECT_EQ(-1, target->lstat("/usr/lib/locale/locale-archive", &st));
        ASSERT_EQ(0, target->lstat("/usr/lib/hard", &st));
        EXPECT_EQ(2, st.st_nlink);

        // layer 3: whiteout a lower layer directory, plus an opaque marker
        {
            TarBuilder tb(fs, fn3, prefix);
            ASSERT_TRUE(tb.ok());
            if (!prefix.empty())
                tb.add_dot();
            tb.add_dir("usr");
            tb.add_dir("usr/lib");
            tb.add_file("usr/lib/.wh.gconv", "");
            tb.add_dir("usr/share");
            tb.add_file("usr/share/.wh..wh..opq", "");
            tb.add_file("usr/share/upper-only", "upper payload");
        }
        ASSERT_EQ(0, untar(fn3));
        EXPECT_EQ(-1, target->lstat("/usr/lib/gconv", &st));
        EXPECT_EQ(-1, target->lstat("/usr/share/lower-only", &st));
        EXPECT_EQ(0, target->lstat("/usr/share/upper-only", &st));
        EXPECT_EQ(-1, target->lstat("/usr/lib/.wh.gconv", &st)); // marker not materialized

        // layer 4: a file directly at the image root, for the whiteouts below
        {
            TarBuilder tb(fs, fn4, prefix);
            ASSERT_TRUE(tb.ok());
            tb.add_file("root-extra", "root payload");
        }
        ASSERT_EQ(0, untar(fn4));
        EXPECT_EQ(0, target->lstat("/root-extra", &st));

        // layer 5: a plain whiteout whose target sits at the image root. Its
        // parent is "/" itself, so this drives remove_all() to walk that root:
        // joining it with a slash would give "//root-extra", which the target fs
        // does not resolve, and the entry would quietly survive. The marker for
        // an absent entry is accepted as well: removing something that no layer
        // provides is a no-op, not a failure.
        {
            TarBuilder tb(fs, fn5, prefix);
            ASSERT_TRUE(tb.ok());
            tb.add_file(".wh.root-extra", "");
            tb.add_file(".wh.absent", "");
        }
        ASSERT_EQ(0, untar(fn5));
        EXPECT_EQ(-1, target->lstat("/root-extra", &st));
        EXPECT_EQ(-1, target->lstat("/.wh.root-extra", &st)); // marker not materialized
        EXPECT_EQ(0, target->lstat("/usr/share/upper-only", &st)); // untouched

        // layer 6: an opaque marker at the image root. Its parent is "/" too, so
        // the strip-the-last-slash step must keep that slash -- turning it into an
        // empty path would make the lstat("") fail and sink the whole layer,
        // rather than make the root opaque.
        {
            TarBuilder tb(fs, fn6, prefix);
            ASSERT_TRUE(tb.ok());
            tb.add_file(".wh..wh..opq", "");
        }
        ASSERT_EQ(0, untar(fn6));
        EXPECT_EQ(0, target->lstat("/", &st)); // the root itself stays
        EXPECT_EQ(-1, target->lstat("/usr", &st));
        EXPECT_EQ(-1, target->lstat("/usr/lib/messy", &st));
        EXPECT_EQ(-1, target->lstat("/.wh..wh..opq", &st)); // marker not materialized

        // layer 7: a member that climbs out of the image root. clean_name() folds
        // the double dots away only for a rooted name, so the two flavours part
        // ways here -- an unrooted one still carries them and is turned down, the
        // way the subfs("/") wrapper's path_level_valid() used to turn it down.
        {
            TarBuilder tb(fs, fn7, prefix);
            ASSERT_TRUE(tb.ok());
            tb.add_file("../../escape", "escape payload");
        }
        if (prefix.empty()) {
            EXPECT_NE(0, untar(fn7));
            EXPECT_EQ(-1, target->lstat("/escape", &st));
        } else {
            EXPECT_EQ(0, untar(fn7));
            EXPECT_EQ(0, target->lstat("/escape", &st));
        }
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
    auto verifyfs = new_extfs(verify_dev, false);
    auto turboOCI_verify = new UnTar(src_file, verifyfs, 0, 4096, verify_dev, true);
    ASSERT_EQ(0, turboOCI_verify->extract_all());
    verifyfs->sync();
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
    auto target = new_extfs(imgfile, false);
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
        auto verifyfs = new_extfs(verify_dev, false);
        // gzfile->lseek(0, SEEK_SET);
        auto turboOCI_verify = new UnTar(gzfile, verifyfs, 0, 4096, verify_dev, true);
        ASSERT_EQ(0, turboOCI_verify->extract_all());
        verifyfs->sync();

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
        auto target = new_extfs(imgfile, false);
        auto turboOCI_mock = new UnTar(tar_idx, target, TAR_IGNORE_CRC, 4096, imgfile, true, true);
        auto ret = turboOCI_mock->extract_all();
        target->sync();

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

TEST_F(TarTest, layer_names_unrooted) {
    check_layer_names("");
}

TEST_F(TarTest, layer_names_rooted) {
    check_layer_names("/");
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

TEST(CleanNameTest, rooted_name) {
    // names are cleaned by clean_name() before they get here, so a rooted one
    // carries neither an empty element nor a ".." and is taken as is
    EXPECT_EQ("/tar_test/busybox", rooted_name("/tar_test/busybox"));
    EXPECT_EQ("/", rooted_name("/"));
    // an unrooted name gets exactly one leading slash, never two -- prepending it
    // unconditionally is what the subfs("/") wrapper got wrong
    EXPECT_EQ("/tar_test/busybox", rooted_name("tar_test/busybox"));
    EXPECT_EQ("/busybox", rooted_name("busybox"));
    // clean_name() maps both "" and "./" to ".", which subfs("/") rooted the same
    // way; extract_dir() then just finds the root already there
    EXPECT_EQ("/.", rooted_name("."));
    // a name that climbs above the root is not rooted but turned down, and the
    // check is the very one subfs used. clean_name() keeps a ".." only at the
    // front of an unrooted name, so that is all this has to catch
    EXPECT_FALSE(photon::fs::path_level_valid(".."));
    EXPECT_FALSE(photon::fs::path_level_valid("../busybox"));
    EXPECT_FALSE(photon::fs::path_level_valid("../../busybox"));
    EXPECT_TRUE(photon::fs::path_level_valid("tar_test/busybox"));
    EXPECT_TRUE(photon::fs::path_level_valid("/tar_test/busybox"));
}

TEST(PathUtilTest, remove_last_slash) {
    // the root is all slash: stripping the last one would leave an empty path,
    // which an opaque whiteout at the root would hand to lstat() as ""
    EXPECT_EQ("/", remove_last_slash("/"));
    EXPECT_EQ("/usr/share", remove_last_slash("/usr/share/"));
    EXPECT_EQ("/usr/share", remove_last_slash("/usr/share"));
    EXPECT_EQ("/usr", remove_last_slash("/usr/"));
}

int main(int argc, char **argv) {

    ::testing::InitGoogleTest(&argc, argv);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    set_log_output_level(1);

    auto ret = RUN_ALL_TESTS();
    (void)ret;

    return 0;
}
