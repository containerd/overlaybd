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
#include "../prefetch.cpp"

#include "../tools/comm_func.h"
#include "../tools/sha256file.h"
#include "photon/common/utility.h"

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
    auto fs = create_ext4fs(dst, true, false, "/");
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


int main(int argc, char **arg) {
    photon::init();
    DEFER(photon::fini());

    set_log_output_level(ALOG_INFO);
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}