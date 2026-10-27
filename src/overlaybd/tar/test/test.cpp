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
#include <dirent.h>
#include <fcntl.h>
#include <photon/photon.h>
#include <photon/fs/localfs.h>
#include <photon/fs/path.h>
#include <photon/fs/subfs.h>
#include <photon/fs/extfs/extfs.h>
#include <photon/fs/forwardfs.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <zlib.h>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <vector>
#include "../../gzindex/gzfile.h"
#include "../../lsmt/file.h"
#include "../libtar.h"
#include "../fsmeta_file.h"
#include "../tar_file.cpp"
#include "../../gzip/gz.h"
#include "../../../tools/sha256file.h"
#include <linux/falloc.h>


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
    // pax records for the entry that follows
    void add_pax(const std::vector<std::pair<std::string, std::string>> &records) {
        std::string content;
        for (auto &r : records) {
            // the length of a record counts its own digits too
            auto body = " " + r.first + "=" + r.second + "\n";
            auto len = body.size() + 1;
            while (std::to_string(len).size() + body.size() != len)
                len = std::to_string(len).size() + body.size();
            content += std::to_string(len) + body;
        }
        put("././@PaxHeader", PAX_HEADER, 0644, "", content);
    }
    // a GNU long name for the entry that follows
    void add_gnu_longname(const std::string &name) {
        put("././@LongLink", GNU_LONGNAME_TYPE, 0644, "", name + '\0');
    }
    // the header claims hdr_size, if given, rather than the size of the content
    void add_entry(const std::string &name, char typeflag, mode_t mode,
                   const std::string &linkname, const std::string &content,
                   ssize_t hdr_size = -1) {
        put(m_prefix + name, typeflag, mode, linkname, content, hdr_size);
    }
    size_t data_bytes() const {
        return m_data_bytes;
    }

private:
    void put(const std::string &name, char typeflag, mode_t mode,
             const std::string &linkname, const std::string &content, ssize_t hdr_size = -1) {
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
        int_to_oct(hdr_size < 0 ? content.size() : (size_t)hdr_size, h.size, (int)sizeof(h.size));
        int_to_oct(0, h.mtime, (int)sizeof(h.mtime));
        if (typeflag == REGTYPE)
            m_data_bytes += (content.size() + FS_BLOCKSIZE - 1) / FS_BLOCKSIZE * FS_BLOCKSIZE;

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
                memset(block, typeflag == REGTYPE ? 0x5a : 0, T_BLOCKSIZE);
                memcpy(block, content.data() + off, n);
                m_file->write(block, T_BLOCKSIZE);
                off += n;
            }
        }
    }

    photon::fs::IFile *m_file = nullptr;
    std::string m_prefix;
    size_t m_data_bytes = 0;
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

namespace {

uint64_t pad_to(uint64_t x, uint64_t alignment) {
    return (x + alignment - 1) / alignment * alignment;
}

// reads the way a pipe does: neither seeking nor positional reads
class PipeLikeFile : public photon::fs::ForwardFile_Ownership {
public:
    explicit PipeLikeFile(photon::fs::IFile *file) : photon::fs::ForwardFile_Ownership(file, true) {
    }
    off_t lseek(off_t, int) override {
        errno = ESPIPE;
        return -1;
    }
    ssize_t pread(void *, size_t, off_t) override {
        errno = ESPIPE;
        return -1;
    }
};

std::string payload(size_t size, uint32_t seed) {
    std::string s(size, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (auto &c : s) {
        x = x * 1103515245 + 12345;
        c = x >> 16;
    }
    return s;
}

std::string slurp(photon::fs::IFileSystem *fs, const std::string &path) {
    std::unique_ptr<photon::fs::IFile> f(fs->open(path.c_str(), O_RDONLY));
    if (!f) {
        ADD_FAILURE() << "failed to open " << path;
        return {};
    }
    std::string content;
    std::vector<char> buf(64 << 10);
    ssize_t n;
    while ((n = f->read(buf.data(), buf.size())) > 0)
        content.append(buf.data(), n);
    EXPECT_EQ(0, n) << path;
    return content;
}

uint64_t fnv1a(const std::string &s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

int gzip_to(photon::fs::IFileSystem *fs, const std::string &src, const std::string &dst) {
    auto content = slurp(fs, src);
    gzFile gz = gzopen(dst.c_str(), "wb");
    if (gz == nullptr)
        return -1;
    auto n = gzwrite(gz, content.data(), content.size());
    return (gzclose(gz) == Z_OK && n == (int)content.size()) ? 0 : -1;
}

std::string child_path(const std::string &dir, const std::string &name) {
    return (dir == "/" ? std::string() : dir) + "/" + name;
}

std::vector<std::string> dir_entries(photon::fs::IFileSystem *fs, const std::string &dir) {
    std::vector<std::string> names;
    auto d = fs->opendir(dir.c_str());
    if (d == nullptr) {
        ADD_FAILURE() << "failed to open dir " << dir;
        return names;
    }
    while (auto de = d->readdir()) {
        std::string name = de->d_name;
        if (name != "." && name != "..")
            names.push_back(name);
    }
    fs->closedir(d);
    std::sort(names.begin(), names.end());
    return names;
}

// path -> what is observable about the entry, the content of a regular file included
using Manifest = std::map<std::string, std::string>;

void describe_tree(photon::fs::IFileSystem *fs, const std::string &dir, Manifest &manifest,
                   std::map<ino_t, std::string> &links) {
    auto xfs = dynamic_cast<photon::fs::IFileSystemXAttr *>(fs);
    for (auto &name : dir_entries(fs, dir)) {
        auto path = child_path(dir, name);
        struct stat st;
        if (fs->lstat(path.c_str(), &st) != 0) {
            ADD_FAILURE() << "failed to lstat " << path;
            continue;
        }
        std::ostringstream os;
        os << std::oct << st.st_mode << std::dec << " " << st.st_uid << ":" << st.st_gid
           << " nlink=" << st.st_nlink;
        // a directory changed by later entries keeps the time of the change
        if (!S_ISDIR(st.st_mode)) {
            os << " mtime=" << st.st_mtime;
            if (st.st_nlink > 1)
                os << " link=" << links.emplace(st.st_ino, path).first->second;
        }
        if (S_ISREG(st.st_mode)) {
            os << " size=" << st.st_size << " fnv=" << std::hex << fnv1a(slurp(fs, path))
               << std::dec;
        } else if (S_ISLNK(st.st_mode)) {
            char target[4096];
            auto n = fs->readlink(path.c_str(), target, sizeof(target));
            // the extfs counts a terminating NUL in
            os << " target=" << std::string(target, strnlen(target, n > 0 ? n : 0));
        }
        if (xfs) {
            char list[4096];
            auto n = xfs->llistxattr(path.c_str(), list, sizeof(list));
            std::vector<std::string> xnames;
            for (ssize_t i = 0; i < n; i += strlen(list + i) + 1)
                xnames.emplace_back(list + i);
            std::sort(xnames.begin(), xnames.end());
            for (auto &x : xnames) {
                char value[4096];
                auto m = xfs->lgetxattr(path.c_str(), x.c_str(), value, sizeof(value));
                os << " " << x << "=" << std::string(value, m > 0 ? m : 0);
            }
        }
        manifest[path] = os.str();
        if (S_ISDIR(st.st_mode))
            describe_tree(fs, path, manifest, links);
    }
}

Manifest describe_tree(photon::fs::IFileSystem *fs) {
    Manifest manifest;
    std::map<ino_t, std::string> links;
    describe_tree(fs, "/", manifest, links);
    return manifest;
}

void compare_trees(const Manifest &expected, const Manifest &actual) {
    for (auto &e : expected) {
        auto it = actual.find(e.first);
        if (it == actual.end())
            ADD_FAILURE() << "missing " << e.first;
        else
            EXPECT_EQ(e.second, it->second) << e.first;
    }
    for (auto &a : actual) {
        if (!expected.count(a.first))
            ADD_FAILURE() << "unexpected " << a.first;
    }
}

struct FileBlocks {
    size_t size = 0;
    std::vector<std::pair<uint64_t, uint64_t>> data, tails; // <offset in the image, length>
};
using Files = std::map<ino_t, FileBlocks>;

void file_blocks(photon::fs::IFileSystem *fs, const std::string &dir, Files &files) {
    for (auto &name : dir_entries(fs, dir)) {
        auto path = child_path(dir, name);
        struct stat st;
        ASSERT_EQ(0, fs->lstat(path.c_str(), &st)) << path;
        if (S_ISDIR(st.st_mode)) {
            file_blocks(fs, path, files);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            continue;
        }
        if (!S_ISREG(st.st_mode) || st.st_size == 0 || files.count(st.st_ino))
            continue;
        std::unique_ptr<photon::fs::IFile> f(fs->open(path.c_str(), O_RDONLY));
        ASSERT_NE(nullptr, f) << path;
        std::unique_ptr<photon::fs::fiemap_t<1024>> fie(
            new photon::fs::fiemap_t<1024>(0, st.st_size));
        ASSERT_EQ(0, f->fiemap(fie.get())) << path;
        ASSERT_LT(fie->fm_mapped_extents, 1024u) << path;
        auto &fb = files[st.st_ino];
        fb.size = st.st_size;
        // the extents come in file order, without fe_logical
        uint64_t data_len = pad_to(st.st_size, FS_BLOCKSIZE), pos = 0;
        uint64_t tail_begin = pad_to(st.st_size, T_BLOCKSIZE);
        for (uint32_t i = 0; i < fie->fm_mapped_extents; i++) {
            auto &e = fie->fm_extents[i];
            uint64_t len = pos < data_len ? std::min<uint64_t>(e.fe_length, data_len - pos) : 0;
            if (len > 0)
                fb.data.emplace_back(e.fe_physical, len);
            uint64_t tail = pos < tail_begin ? std::min<uint64_t>(e.fe_length, tail_begin - pos) : 0;
            if (tail < e.fe_length)
                fb.tails.emplace_back(e.fe_physical + tail, e.fe_length - tail);
            pos += e.fe_length;
        }
        EXPECT_GE(pos, data_len) << path;
    }
}

Files file_blocks(photon::fs::IFileSystem *fs) {
    Files files;
    file_blocks(fs, "/", files);
    return files;
}

// Checks the sealed top layer of `layers`: the file data staged at [4096, payload_end),
// the fs metadata after it up to data_end, then the index and the trailer. Of `files`,
// those whose data is mapped by the top layer must be single runs of the staged data;
// there must be `staged_files` of them.
void check_staged_layout(std::vector<photon::fs::IFile *> layers, off_t payload_end,
                         off_t data_end, const Files &files, size_t staged_files) {
    auto top = layers.back();
    std::unique_ptr<LSMT::IFileRO> ro(LSMT::open_file_ro(top, false));
    ASSERT_NE(nullptr, ro);
    auto index = ro->index();
    auto n = index->size();
    ASSERT_GT(n, 0UL);
    struct stat st;
    ASSERT_EQ(0, top->fstat(&st));
    auto bytes = n * sizeof(LSMT::SegmentMapping);
    EXPECT_EQ(data_end + (off_t)pad_to(bytes, T_BLOCKSIZE) + 4096, st.st_size);
    // as stored, i.e. without a tag
    std::vector<LSMT::SegmentMapping> stored(n);
    ASSERT_EQ((ssize_t)bytes, top->pread(stored.data(), bytes, data_end));
    EXPECT_EQ(0, memcmp(stored.data(), index->buffer(), bytes));

    uint64_t end = 0, payload_bytes = 0, meta_bytes = 0;
    for (size_t i = 0; i < n; i++) {
        auto &m = index->buffer()[i];
        EXPECT_LE(end, (uint64_t)m.offset);
        end = m.end();
        if (m.zeroed)
            continue;
        uint64_t mbegin = m.moffset * T_BLOCKSIZE, mend = m.mend() * T_BLOCKSIZE;
        EXPECT_LE(4096UL, mbegin);
        EXPECT_LE(mend, (uint64_t)data_end);
        if (mend <= (uint64_t)payload_end)
            payload_bytes += m.length * T_BLOCKSIZE;
        else if (mbegin >= (uint64_t)payload_end)
            meta_bytes += m.length * T_BLOCKSIZE;
        else
            ADD_FAILURE() << "a mapping straddles the end of the file data: [" << mbegin << ", "
                          << mend << ")";
    }
    EXPECT_GT(payload_bytes, 0UL);
    EXPECT_GT(meta_bytes, 0UL);

    std::vector<std::pair<int64_t, int64_t>> runs;
    std::vector<LSMT::SegmentMapping> pm(8192);
    for (auto &it : files) {
        auto &fb = it.second;
        uint64_t data_len = pad_to(fb.size, FS_BLOCKSIZE), pos = 0, mapped = 0;
        int64_t base = INT64_MIN;
        bool single_run = true;
        for (auto &piece : fb.data) {
            for (uint64_t off = 0; off < piece.second; off += 4 << 20) {
                auto len = std::min<uint64_t>(4 << 20, piece.second - off);
                LSMT::Segment s{(piece.first + off) / T_BLOCKSIZE, (uint32_t)(len / T_BLOCKSIZE)};
                auto cnt = index->lookup(s, pm.data(), pm.size());
                for (size_t j = 0; j < cnt; j++) {
                    auto &x = pm[j];
                    uint64_t file_pos = pos + x.offset * T_BLOCKSIZE - piece.first;
                    int64_t b = x.zeroed ? -1 : (int64_t)(x.moffset * T_BLOCKSIZE) - (int64_t)file_pos;
                    if (base == INT64_MIN)
                        base = b;
                    single_run &= (b == base);
                    mapped += x.length * T_BLOCKSIZE;
                }
            }
            pos += piece.second;
        }
        if (mapped == 0)
            continue; // staged in a lower layer
        EXPECT_EQ(data_len, mapped) << "inode " << it.first;
        EXPECT_TRUE(single_run) << "inode " << it.first;
        EXPECT_LE(4096L, base) << "inode " << it.first;
        EXPECT_EQ(0L, base % FS_BLOCKSIZE) << "inode " << it.first;
        EXPECT_LE(base + (int64_t)data_len, (int64_t)payload_end) << "inode " << it.first;
        if (fb.size % FS_BLOCKSIZE != 0) {
            std::vector<char> last(FS_BLOCKSIZE);
            ASSERT_EQ(FS_BLOCKSIZE, top->pread(last.data(), last.size(), base + data_len - FS_BLOCKSIZE));
            EXPECT_TRUE(std::all_of(last.begin() + fb.size % FS_BLOCKSIZE, last.end(),
                                    [](char c) { return c == 0; })) << "inode " << it.first;
        }
        runs.emplace_back(base, base + data_len);
    }
    EXPECT_EQ(staged_files, runs.size());
    std::sort(runs.begin(), runs.end());
    for (size_t i = 1; i < runs.size(); i++)
        EXPECT_LE(runs[i - 1].second, runs[i].first);

    // the slack of a last block reads as zero, even if the block held something else before
    std::unique_ptr<LSMT::IFileRO> image(LSMT::open_files_ro(layers.data(), layers.size(), false));
    ASSERT_NE(nullptr, image);
    std::vector<char> buf, zero;
    for (auto &it : files) {
        for (auto &t : it.second.tails) {
            buf.resize(t.second);
            zero.assign(t.second, 0);
            ASSERT_EQ((ssize_t)t.second, image->pread(buf.data(), t.second, t.first));
            EXPECT_EQ(0, memcmp(buf.data(), zero.data(), t.second))
                << "stale data in the last block of inode " << it.first << " at " << t.first;
        }
    }
}

} // namespace

class StagedTarTest : public TarTest, public ::testing::WithParamInterface<bool> {};

TEST_P(StagedTarTest, staged_extract) {
    for (auto fn : {"staged_1.tar", "staged_2.tar", "staged_2.tar.gz", "staged_1.spool",
                    "staged_2.spool", "staged_1.data", "staged_1.idx", "staged_2.data",
                    "staged_2.idx", "staged_scratch.data", "staged_scratch.idx", "staged_ref.img",
                    "staged_meta.sparse"})
        filelist.push_back(fn);

    uint32_t seed = 0;
    using Contents = std::map<std::string, std::string>;
    auto add = [&](TarBuilder &tb, Contents &files, const std::string &name, size_t size) {
        auto content = payload(size, ++seed);
        tb.add_file(name, content);
        files["/" + name] = content;
    };

    Contents files1, files2;
    size_t data_bytes1, data_bytes2;
    const std::string pax_path = "pax/" + std::string(150, 'x');
    const std::string gnu_path = "gnu/" + std::string(140, 'g');
    {
        TarBuilder tb(fs, "staged_1.tar", "");
        ASSERT_TRUE(tb.ok());
        tb.add_dir("data");
        for (size_t size : {0, 1, 511, 512, 513, 4095, 4096, 4097})
            add(tb, files1, "data/s" + std::to_string(size), size);
        tb.add_dir("gone");
        add(tb, files1, "gone/x", 4096);
        // emptied again, they leave holes for layer 2 to fill
        tb.add_dir("frag");
        for (int i = 0; i < 64; i++)
            add(tb, files1, "frag/h" + std::to_string(i), 4096);
        for (int i = 1; i < 64; i += 2)
            add(tb, files1, "frag/h" + std::to_string(i), 0);
        // more than a mapping can hold
        add(tb, files1, "big", (9 << 20) + 123);
        tb.add_dir("pax");
        auto content = payload(5000, ++seed);
        // the size of the pax record rules
        tb.add_pax({{"path", pax_path}, {"SCHILY.xattr.user.origin", "pax"}, {"size", "5000"}});
        tb.add_entry(pax_path, REGTYPE, 0640, "", content, 0);
        files1["/" + pax_path] = content;
        tb.add_dir("gnu");
        content = payload(70000, ++seed);
        tb.add_gnu_longname(gnu_path);
        tb.add_entry(gnu_path, REGTYPE, 0644, "", content);
        files1["/" + gnu_path] = content;
        tb.add_hardlink("data/link-to-4097", "data/s4097");
        files1["/data/link-to-4097"] = files1["/data/s4097"];
        tb.add_symlink("data/sym", "s4096");
        add(tb, files1, "data/dup", 10000);
        add(tb, files1, "data/dup", 3000);
        // replaced, but still linked
        add(tb, files1, "data/orig", 6000);
        tb.add_hardlink("data/keep", "data/orig");
        files1["/data/keep"] = files1["/data/orig"];
        add(tb, files1, "data/orig", 100);
        tb.add_dir("opq");
        add(tb, files1, "opq/lower", 2000);
        data_bytes1 = tb.data_bytes();
    }
    {
        files2 = files1;
        for (auto it = files2.begin(); it != files2.end();) {
            if (it->first.rfind("/frag/", 0) == 0 || it->first.rfind("/gone/", 0) == 0 ||
                it->first == "/data/s4095" || it->first == "/opq/lower")
                it = files2.erase(it);
            else
                ++it;
        }
        TarBuilder tb(fs, "staged_2.tar", "");
        ASSERT_TRUE(tb.ok());
        // a mounted fs allocates blocks from its start, each time going on from where it
        // left off: the first files get the blocks of s4095 and of the replaced s4096,
        // which hold more data in layer 1 than the new files do
        tb.add_dir("data");
        tb.add_file("data/.wh.s4095", "");
        add(tb, files2, "data/s4096", 100);
        tb.add_dir("opq");
        tb.add_file("opq/.wh..wh..opq", "");
        add(tb, files2, "opq/after-opq", 2048);
        // big2 gets the block of dir gone, changed and freed but maybe not written back
        // yet, then the holes layer 1 left between frag/h*, and more
        tb.add_file(".wh.gone", "");
        add(tb, files2, "big2", 300000);
        tb.add_file(".wh.frag", "");
        tb.add_hardlink("data/l2-link", "data/s1");
        files2["/data/l2-link"] = files2["/data/s1"];
        data_bytes2 = tb.data_bytes();
    }
    EXPECT_EQ(311296UL, data_bytes2);
    ASSERT_EQ(0, gzip_to(fs, "staged_2.tar", workdir + "/staged_2.tar.gz"));

    auto open_new = [&](const char *fn) {
        std::unique_ptr<photon::fs::IFile> f(fs->open(fn, O_RDWR | O_CREAT | O_TRUNC, 0644));
        EXPECT_NE(nullptr, f) << fn;
        return f;
    };
    auto file_size = [](photon::fs::IFile *f) {
        struct stat st;
        EXPECT_EQ(0, f->fstat(&st));
        return st.st_size;
    };
    auto create_layer = [](photon::fs::IFile *fdata, photon::fs::IFile *findex) {
        LSMT::LayerInfo args(fdata, findex);
        args.virtual_size = IMAGE_SIZE;
        return std::unique_ptr<LSMT::IFileRW>(LSMT::create_file_rw(args, false));
    };
    auto replay = [&](photon::fs::IFile *headers, LSMT::IFileRW *img, Files &blocks, bool mkfs) {
        ASSERT_EQ(0, headers->lseek(0, SEEK_SET));
        auto sparse = open_new("staged_meta.sparse");
        ASSERT_NE(nullptr, sparse);
        FsMetaFile fsmeta(img, sparse.get(), IMAGE_SIZE);
        auto backend = GetParam() ? (photon::fs::IFile *)&fsmeta : img;
        if (mkfs) {
            ASSERT_EQ(0, make_extfs(backend));
        }
        std::unique_ptr<photon::fs::IFileSystem> vfs(
            new_extfs_with_buffer(backend, GetParam() ? 0 : 64 << 10, 4 << 10));
        ASSERT_NE(nullptr, vfs);
        UnTar tar(headers, vfs.get(), 0, 4096, img);
        if (GetParam()) {
            auto data_end = file_size(img->get_lower_files().back());
            ASSERT_EQ(0, tar.extract_staged(&fsmeta, [&]() {
                EXPECT_EQ(data_end, file_size(img->get_lower_files().back()));
                blocks = file_blocks(vfs.get());
                vfs.reset();
            }));
        } else {
            ASSERT_EQ(0, tar.extract_staged());
            vfs.reset(new_extfs(img, false));
            ASSERT_NE(nullptr, vfs);
            blocks = file_blocks(vfs.get());
        }
    };
    auto check_compacted = [&](photon::fs::IFile *data, off_t payload_end, off_t data_end) {
        if (!GetParam()) {
            return;
        }
        std::unique_ptr<LSMT::IFileRO> ro(LSMT::open_file_ro(data, false));
        ASSERT_NE(nullptr, ro);
        std::vector<LSMT::SegmentMapping> meta;
        auto index = ro->index();
        for (size_t i = 0; i < index->size(); i++) {
            auto m = index->buffer()[i];
            if (!m.zeroed && m.moffset * T_BLOCKSIZE >= (uint64_t)payload_end) {
                meta.push_back(m);
            }
        }
        std::sort(meta.begin(), meta.end(), [](const auto &a, const auto &b) {
            return a.moffset < b.moffset;
        });
        uint64_t physical_end = payload_end, logical_end = 0;
        for (auto &m : meta) {
            EXPECT_EQ(physical_end, m.moffset * T_BLOCKSIZE);
            EXPECT_LE(logical_end, (uint64_t)m.offset);
            physical_end = m.mend() * T_BLOCKSIZE;
            logical_end = m.end();
        }
        EXPECT_EQ((uint64_t)data_end, physical_end);
    };
    // as `overlaybd-commit --seal` does
    auto seal = [](photon::fs::IFile *fdata, photon::fs::IFile *findex) {
        std::unique_ptr<LSMT::IFileRW> rw(LSMT::open_file_rw(fdata, findex, false));
        ASSERT_NE(nullptr, rw);
        ASSERT_EQ(0, rw->close_seal());
    };

    // layer 1, from a seekable tar
    auto d1 = open_new("staged_1.data"), i1 = open_new("staged_1.idx");
    auto spool1 = open_new("staged_1.spool");
    ASSERT_TRUE(d1 && i1 && spool1);
    off_t payload_end1, data_end1;
    Files blocks1;
    {
        auto rw = create_layer(d1.get(), i1.get());
        ASSERT_NE(nullptr, rw);
        std::unique_ptr<photon::fs::IFile> src(fs->open("staged_1.tar", O_RDONLY));
        ASSERT_NE(nullptr, src);
        UnTar scan(src.get(), nullptr, 0, 4096);
        EXPECT_LT(0, scan.stage_tar(spool1.get(), rw->get_lower_files().back()));
        payload_end1 = file_size(d1.get());
        EXPECT_EQ((off_t)(4096 + pad_to(data_bytes1, 4096)), payload_end1);
        // nothing is mapped yet
        EXPECT_EQ(0UL, rw->index()->size());
        replay(spool1.get(), rw.get(), blocks1, true);
        ASSERT_FALSE(HasFatalFailure());
    }
    data_end1 = file_size(d1.get());
    seal(d1.get(), i1.get());
    ASSERT_FALSE(HasFatalFailure());
    auto sealed1 = file_size(d1.get());
    check_staged_layout({d1.get()}, payload_end1, data_end1, blocks1, blocks1.size());
    check_compacted(d1.get(), payload_end1, data_end1);
    ASSERT_FALSE(HasFatalFailure());

    // layer 2, from a gzipped stream on top of layer 1
    auto d2 = open_new("staged_2.data"), i2 = open_new("staged_2.idx");
    auto spool2 = open_new("staged_2.spool");
    ASSERT_TRUE(d2 && i2 && spool2);
    off_t payload_end2, data_end2;
    Files blocks2;
    {
        auto rw = create_layer(d2.get(), i2.get());
        ASSERT_NE(nullptr, rw);
        std::unique_ptr<LSMT::IFileRO> lower(LSMT::open_file_ro(d1.get(), false));
        ASSERT_NE(nullptr, lower);
        std::unique_ptr<LSMT::IFileRW> img(LSMT::stack_files(rw.get(), lower.get(), false, false));
        ASSERT_NE(nullptr, img);
        auto gz = open_gzfile_adaptor((workdir + "/staged_2.tar.gz").c_str());
        ASSERT_NE(nullptr, gz);
        PipeLikeFile src(gz);
        UnTar scan(&src, nullptr, 0, 4096);
        EXPECT_LT(0, scan.stage_tar(spool2.get(), img->get_lower_files().back()));
        payload_end2 = file_size(d2.get());
        EXPECT_EQ((off_t)(4096 + pad_to(data_bytes2, 4096)), payload_end2);
        replay(spool2.get(), img.get(), blocks2, false);
        ASSERT_FALSE(HasFatalFailure());
    }
    EXPECT_EQ(sealed1, file_size(d1.get()));
    data_end2 = file_size(d2.get());
    seal(d2.get(), i2.get());
    ASSERT_FALSE(HasFatalFailure());
    check_staged_layout({d1.get(), d2.get()}, payload_end2, data_end2, blocks2, 3);
    check_compacted(d2.get(), payload_end2, data_end2);
    ASSERT_FALSE(HasFatalFailure());

    // what extracting the layers directly gives
    Manifest ref1, ref2;
    {
        auto raw = open_new("staged_ref.img");
        ASSERT_NE(nullptr, raw);
        ASSERT_EQ(0, raw->ftruncate(IMAGE_SIZE));
        ASSERT_EQ(0, make_extfs(raw.get()));
        std::unique_ptr<photon::fs::IFileSystem> ref(new_extfs(raw.get(), false));
        ASSERT_NE(nullptr, ref);
        auto extract = [&](const char *fn, Manifest &manifest) {
            std::unique_ptr<photon::fs::IFile> tarf(fs->open(fn, O_RDONLY));
            ASSERT_NE(nullptr, tarf);
            UnTar tar(tarf.get(), ref.get(), 0, 4096);
            ASSERT_EQ(0, tar.extract_all());
            manifest = describe_tree(ref.get());
        };
        extract("staged_1.tar", ref1);
        ASSERT_FALSE(HasFatalFailure());
        extract("staged_2.tar", ref2);
        ASSERT_FALSE(HasFatalFailure());
    }

    auto view = [&](std::vector<photon::fs::IFile *> layers, const Manifest &expected,
                    const Contents &files, const std::function<void(photon::fs::IFileSystem *)> &check) {
        std::unique_ptr<LSMT::IFileRO> ro(LSMT::open_files_ro(layers.data(), layers.size(), false));
        ASSERT_NE(nullptr, ro);
        // the fs writes its superblock when unmounted
        auto sd = open_new("staged_scratch.data"), si = open_new("staged_scratch.idx");
        ASSERT_TRUE(sd && si);
        auto scratch = create_layer(sd.get(), si.get());
        ASSERT_NE(nullptr, scratch);
        std::unique_ptr<LSMT::IFileRW> img(LSMT::stack_files(scratch.get(), ro.get(), false, false));
        ASSERT_NE(nullptr, img);
        std::unique_ptr<photon::fs::IFileSystem> vfs(new_extfs(img.get(), false));
        ASSERT_NE(nullptr, vfs);
        compare_trees(expected, describe_tree(vfs.get()));
        for (auto &f : files)
            EXPECT_TRUE(slurp(vfs.get(), f.first) == f.second) << f.first;
        check(vfs.get());
    };

    view({d1.get()}, ref1, files1, [&](photon::fs::IFileSystem *vfs) {
        struct stat st;
        // an extent of big is longer than a mapping can be
        ASSERT_EQ(0, vfs->lstat("/big", &st));
        ASSERT_EQ(1UL, blocks1.count(st.st_ino));
        uint64_t longest = 0;
        for (auto &piece : blocks1[st.st_ino].data)
            longest = std::max(longest, piece.second);
        EXPECT_LT((uint64_t)LSMT::Segment::MAX_LENGTH * T_BLOCKSIZE, longest);
        auto pax = "/" + pax_path;
        ASSERT_EQ(0, vfs->lstat(pax.c_str(), &st));
        EXPECT_EQ(0640U, st.st_mode & 07777);
        EXPECT_EQ(5000, st.st_size);
        auto xfs = dynamic_cast<photon::fs::IFileSystemXAttr *>(vfs);
        ASSERT_NE(nullptr, xfs);
        char value[16] = {0};
        EXPECT_EQ(3, xfs->lgetxattr(pax.c_str(), "user.origin", value, sizeof(value)));
        EXPECT_STREQ("pax", value);
    });
    ASSERT_FALSE(HasFatalFailure());

    view({d1.get(), d2.get()}, ref2, files2, [&](photon::fs::IFileSystem *vfs) {
        struct stat st, st2;
        for (auto gone : {"/frag", "/gone", "/data/s4095", "/opq/lower"})
            EXPECT_EQ(-1, vfs->lstat(gone, &st)) << gone;
        std::unique_ptr<LSMT::IFileRO> lower(LSMT::open_file_ro(d1.get(), false));
        ASSERT_NE(nullptr, lower);
        // with more extents than an inode holds, the first one on fs metadata of layer 1
        ASSERT_EQ(0, vfs->lstat("/big2", &st));
        ASSERT_EQ(1UL, blocks2.count(st.st_ino));
        auto &big2 = blocks2[st.st_ino].data;
        EXPECT_LT(4UL, big2.size());
        LSMT::SegmentMapping m;
        ASSERT_EQ(1UL, lower->index()->lookup(LSMT::Segment{big2[0].first / T_BLOCKSIZE, 1}, &m, 1));
        EXPECT_LE((uint64_t)payload_end1, m.moffset * T_BLOCKSIZE);
        // the slack of their last blocks, checked to read as zero, has data in layer 1
        for (auto reused : {"/data/s4096", "/opq/after-opq"}) {
            ASSERT_EQ(0, vfs->lstat(reused, &st));
            ASSERT_EQ(1UL, blocks2.count(st.st_ino));
            auto &tails = blocks2[st.st_ino].tails;
            ASSERT_EQ(1UL, tails.size()) << reused;
            std::string stale(tails[0].second, '\0');
            ASSERT_EQ((ssize_t)stale.size(), lower->pread(&stale[0], stale.size(), tails[0].first));
            EXPECT_NE(std::string::npos, stale.find_first_not_of('\0')) << reused;
        }
        ASSERT_EQ(0, vfs->lstat("/data/link-to-4097", &st));
        ASSERT_EQ(0, vfs->lstat("/data/s4097", &st2));
        EXPECT_EQ(st2.st_ino, st.st_ino);
        ASSERT_EQ(0, vfs->lstat("/data/l2-link", &st));
        ASSERT_EQ(0, vfs->lstat("/data/s1", &st2));
        EXPECT_EQ(st2.st_ino, st.st_ino);
        char target[64] = {0};
        ASSERT_LT(0, vfs->readlink("/data/sym", target, sizeof(target)));
        EXPECT_STREQ("s4096", target);
        ASSERT_EQ(0, vfs->lstat("/data/keep", &st));
        EXPECT_EQ(6000, st.st_size);
        ASSERT_EQ(0, vfs->lstat("/data/orig", &st));
        EXPECT_EQ(100, st.st_size);
    });
}

INSTANTIATE_TEST_SUITE_P(FsMeta, StagedTarTest, ::testing::Bool());

class FsMetaTest : public TarTest {
protected:
    class TrackingFile : public photon::fs::ForwardFile {
    public:
        explicit TrackingFile(photon::fs::IFile *file) : ForwardFile(file) {}
        std::vector<std::pair<off_t, size_t>> writes;
        off_t fail_offset = -1;
        off_t short_offset = -1;

        ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
            if (offset == fail_offset) {
                errno = EIO;
                return -1;
            }
            if (offset == short_offset) {
                return m_file->pwrite(buf, count / 2, offset);
            }
            auto ret = m_file->pwrite(buf, count, offset);
            if (ret == (ssize_t)count) {
                writes.emplace_back(offset, count);
            }
            return ret;
        }
    };

    void SetUp() override {
        TarTest::SetUp();
        filelist.push_back("fsmeta_base.img");
        filelist.push_back("fsmeta_staging.img");
        base.reset(fs->open("fsmeta_base.img", O_RDWR | O_CREAT | O_TRUNC, 0644));
        sparse.reset(fs->open("fsmeta_staging.img", O_RDWR | O_CREAT | O_TRUNC, 0644));
        ASSERT_TRUE(base && sparse);
        ASSERT_EQ(0, base->ftruncate(IMAGE_SIZE));
        tracked.reset(new TrackingFile(base.get()));
        overlay.reset(new FsMetaFile(tracked.get(), sparse.get(), IMAGE_SIZE));
    }

    std::unique_ptr<photon::fs::IFile> base, sparse;
    std::unique_ptr<TrackingFile> tracked;
    std::unique_ptr<FsMetaFile> overlay;
};

TEST_F(FsMetaTest, merged_reads_and_partial_writes) {
    std::string initial(5 * 4096, 'a');
    ASSERT_EQ((ssize_t)initial.size(), base->pwrite(initial.data(), initial.size(), 0));
    std::string expected = initial;
    std::string whole(4096, 'b'), partial(5000, 'c');
    ASSERT_EQ((ssize_t)whole.size(), overlay->pwrite(whole.data(), whole.size(), 2 * 4096));
    expected.replace(2 * 4096, whole.size(), whole);
    ASSERT_EQ((ssize_t)partial.size(), overlay->pwrite(partial.data(), partial.size(), 4096 - 123));
    expected.replace(4096 - 123, partial.size(), partial);
    std::string got(initial.size(), '\0');
    ASSERT_EQ((ssize_t)got.size(), overlay->pread(&got[0], got.size(), 0));
    EXPECT_EQ(expected, got);
    ASSERT_EQ(73, overlay->pread(&got[0], 73, 8192 + 17));
    EXPECT_EQ(expected.substr(8192 + 17, 73), got.substr(0, 73));
    ASSERT_EQ((ssize_t)initial.size(), base->pread(&got[0], initial.size(), 0));
    EXPECT_EQ(initial, got);
    EXPECT_TRUE(tracked->writes.empty());
    ASSERT_EQ(0, overlay->commit());
    ASSERT_EQ((ssize_t)got.size(), base->pread(&got[0], got.size(), 0));
    EXPECT_EQ(expected, got);
    ASSERT_EQ(1UL, tracked->writes.size());
    EXPECT_EQ(0, tracked->writes[0].first);
    EXPECT_EQ(3 * 4096UL, tracked->writes[0].second);
    ASSERT_EQ(0, overlay->commit());
    EXPECT_EQ(1UL, tracked->writes.size());
    whole.assign(4096, 'd');
    ASSERT_EQ(4096, overlay->pwrite(whole.data(), whole.size(), 4096));
    ASSERT_EQ(0, overlay->commit());
    EXPECT_EQ(2UL, tracked->writes.size());
}

TEST_F(FsMetaTest, zero_and_discard) {
    std::string expected(4 * 4096, 'a');
    ASSERT_EQ((ssize_t)expected.size(), base->pwrite(expected.data(), expected.size(), 0));
    std::string dirty(3 * 4096, 'b');
    ASSERT_EQ((ssize_t)dirty.size(), overlay->pwrite(dirty.data(), dirty.size(), 0));
    expected.replace(0, dirty.size(), dirty);
    ASSERT_EQ(0, overlay->fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 512, 512));
    expected.replace(512, 512, std::string(512, '\0'));
    ASSERT_EQ(0, overlay->fallocate(FALLOC_FL_ZERO_RANGE, 4096, 4096));
    expected.replace(4096, 4096, std::string(4096, '\0'));
    overlay->discard(8192 + 512, 512);
    expected.replace(8192, 4096, std::string(4096, 'a'));
    std::string got(expected.size(), '\0');
    ASSERT_EQ((ssize_t)got.size(), overlay->pread(&got[0], got.size(), 0));
    EXPECT_EQ(expected, got);
    ASSERT_EQ(0, overlay->commit());
    ASSERT_EQ(1UL, tracked->writes.size());
    EXPECT_EQ(0, tracked->writes[0].first);
    EXPECT_EQ(4096UL, tracked->writes[0].second);
    ASSERT_EQ((ssize_t)got.size(), base->pread(&got[0], got.size(), 0));
    EXPECT_EQ(expected, got);
}

TEST_F(FsMetaTest, sparse_sorted_commit_and_retry) {
    std::string block(4096, 'x');
    for (size_t n : {100000, 64, 0, 63, 62}) {
        ASSERT_EQ(4096, overlay->pwrite(block.data(), block.size(), n * 4096));
    }
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(4096, overlay->pwrite(block.data(), block.size(), 64 * 4096));
    }
    struct stat st;
    ASSERT_EQ(0, sparse->fstat(&st));
    EXPECT_LT((uint64_t)st.st_blocks * 512, (uint64_t)st.st_size / 100);
    EXPECT_TRUE(tracked->writes.empty());
    tracked->fail_offset = 62 * 4096;
    ASSERT_EQ(-1, overlay->commit());
    ASSERT_EQ(1UL, tracked->writes.size());
    tracked->fail_offset = -1;
    ASSERT_EQ(0, overlay->commit());
    ASSERT_EQ(3UL, tracked->writes.size());
    EXPECT_EQ(0, tracked->writes[0].first);
    EXPECT_EQ(62 * 4096, tracked->writes[1].first);
    EXPECT_EQ(3 * 4096UL, tracked->writes[1].second);
    EXPECT_EQ(100000 * 4096, tracked->writes[2].first);
    std::string got(4096, '\0');
    ASSERT_EQ(4096, overlay->pread(&got[0], got.size(), 100000 * 4096));
    EXPECT_EQ(block, got);
}

TEST_F(FsMetaTest, bounds_and_large_run) {
    char byte = 0;
    EXPECT_EQ(-1, overlay->pread(&byte, 1, -1));
    EXPECT_EQ(-1, overlay->pwrite(&byte, 1, IMAGE_SIZE));
    EXPECT_EQ(-1, overlay->fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, IMAGE_SIZE, 4096));
    EXPECT_EQ(-1, overlay->fallocate(0, 0, 4096));
    EXPECT_EQ(0, overlay->pwrite(&byte, 0, IMAGE_SIZE));
    EXPECT_EQ(0, overlay->pread(&byte, 0, IMAGE_SIZE));
    std::string data((1 << 20) + 4096, 'z');
    ASSERT_EQ((ssize_t)data.size(), overlay->pwrite(data.data(), data.size(), 4096));
    ASSERT_EQ(0, overlay->commit());
    ASSERT_EQ(2UL, tracked->writes.size());
    EXPECT_EQ(4096, tracked->writes[0].first);
    EXPECT_EQ(1UL << 20, tracked->writes[0].second);
    EXPECT_EQ((1 << 20) + 4096, tracked->writes[1].first);
    EXPECT_EQ(4096UL, tracked->writes[1].second);
}

TEST_F(FsMetaTest, rejects_failed_staging) {
    for (bool short_write : {false, true}) {
        TrackingFile staging(sparse.get());
        FsMetaFile fsmeta(tracked.get(), &staging, IMAGE_SIZE);
        std::string block(4096, 'a');
        ASSERT_EQ(4096, fsmeta.pwrite(block.data(), block.size(), 0));
        if (short_write) {
            staging.short_offset = 0;
        } else {
            staging.fail_offset = 0;
        }
        block.assign(4096, 'b');
        ASSERT_EQ(-1, fsmeta.pwrite(block.data(), block.size(), 0));
        staging.fail_offset = staging.short_offset = -1;
        ASSERT_EQ(-1, fsmeta.commit());
        EXPECT_TRUE(tracked->writes.empty());
    }
}

TEST_F(FsMetaTest, final_partial_block) {
    size_t size = 64 * 4096 + 512;
    ASSERT_EQ(0, base->ftruncate(size));
    FsMetaFile fsmeta(tracked.get(), sparse.get(), size);
    std::string data(512, 'p'), got(512, '\0');
    ASSERT_EQ(512, base->pwrite(data.data(), data.size(), size - 512));
    ASSERT_EQ(512, fsmeta.pread(&got[0], got.size(), size - 512));
    EXPECT_EQ(data, got);
    char byte = 'q';
    ASSERT_EQ(1, fsmeta.pwrite(&byte, 1, size - 1));
    ASSERT_EQ(512, fsmeta.pread(&got[0], got.size(), size - 512));
    data.back() = byte;
    EXPECT_EQ(data, got);
    ASSERT_EQ(0, fsmeta.commit());
    ASSERT_EQ(512, base->pread(&got[0], got.size(), size - 512));
    EXPECT_EQ(data, got);
    struct stat st;
    ASSERT_EQ(0, base->fstat(&st));
    EXPECT_EQ((off_t)size, st.st_size);
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
