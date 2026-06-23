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
#include <photon/photon.h>
#include <photon/fs/localfs.h>
#include <photon/fs/extfs/extfs.h>
#include <photon/common/alog.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <string>
#include <vector>
#include <sys/statvfs.h>

#define MB (1024ULL * 1024)
#define DATA_PATTERN "abcdefghijklmnopqrstuvwxyz012345"
#define DATA_PATTERN_LEN 32

static std::string TEST_IMG = std::string("/tmp/resize_test_") + std::to_string(::getpid()) + ".img";

static photon::fs::IFile *create_image(uint64_t size_mb) {
    auto file = photon::fs::open_localfile_adaptor(
        TEST_IMG.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644, 0);
    if (!file) return nullptr;
    if (file->ftruncate(size_mb * MB) < 0) {
        delete file;
        return nullptr;
    }
    if (photon::fs::make_extfs(file) < 0) {
        delete file;
        return nullptr;
    }
    return file;
}

static void cleanup() {
    ::unlink(TEST_IMG.c_str());
}

static int write_data_file(photon::fs::IFileSystem *extfs, const char *path,
                           size_t size) {
    auto f = extfs->open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (!f) return -1;
    std::string data;
    while (data.size() < size)
        data.append(DATA_PATTERN, DATA_PATTERN_LEN);
    data.resize(size);
    auto ret = f->pwrite(data.data(), data.size(), 0);
    delete f;
    return (ret == (ssize_t)data.size()) ? 0 : -1;
}

static int verify_data_file(photon::fs::IFileSystem *extfs, const char *path,
                            size_t size) {
    auto f = extfs->open(path, O_RDONLY, 0);
    if (!f) return -1;
    std::string expected;
    while (expected.size() < size)
        expected.append(DATA_PATTERN, DATA_PATTERN_LEN);
    expected.resize(size);
    std::string actual(size, '\0');
    auto ret = f->pread(&actual[0], size, 0);
    delete f;
    if (ret != (ssize_t)size) return -1;
    return (actual == expected) ? 0 : -1;
}

class ResizeTest : public ::testing::Test {
protected:
    void TearDown() override {
        cleanup();
    }
};

TEST_F(ResizeTest, BasicShrinkExpand) {
    auto file = create_image(256);
    ASSERT_NE(nullptr, file);

    auto extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    ASSERT_EQ(0, write_data_file(extfs, "/testdata", 2 * MB));
    delete extfs;

    ASSERT_EQ(0, photon::fs::resize_extfs(file, 128 * MB));

    extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    ASSERT_EQ(0, verify_data_file(extfs, "/testdata", 2 * MB));
    struct statvfs st;
    ASSERT_EQ(0, extfs->statvfs("/", &st));
    EXPECT_LE(st.f_blocks * st.f_bsize, 128 * MB);
    delete extfs;

    file->ftruncate(512 * MB);
    ASSERT_EQ(0, photon::fs::resize_extfs(file, 512 * MB));

    extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    ASSERT_EQ(0, verify_data_file(extfs, "/testdata", 2 * MB));
    ASSERT_EQ(0, extfs->statvfs("/", &st));
    EXPECT_GE(st.f_blocks * st.f_bsize, 400 * MB);
    delete extfs;

    delete file;
}

TEST_F(ResizeTest, ShrinkThenExpand) {
    auto file = create_image(256);
    ASSERT_NE(nullptr, file);

    file->ftruncate(512 * MB);
    ASSERT_EQ(0, photon::fs::resize_extfs(file, 512 * MB));

    ASSERT_EQ(0, photon::fs::resize_extfs(file, 128 * MB));

    file->ftruncate(512 * MB);
    ASSERT_EQ(0, photon::fs::resize_extfs(file, 512 * MB));

    auto extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    ASSERT_EQ(0, write_data_file(extfs, "/after_reexpand", 1 * MB));
    ASSERT_EQ(0, verify_data_file(extfs, "/after_reexpand", 1 * MB));
    delete extfs;

    delete file;
}

TEST_F(ResizeTest, ShrinkWithRelocation) {
    auto file = create_image(256);
    ASSERT_NE(nullptr, file);

    auto extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    extfs->mkdir("/data", 0755);
    for (int i = 0; i < 15; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/data/fill_%03d", i);
        ASSERT_EQ(0, write_data_file(extfs, path, 4 * MB));
    }
    delete extfs;

    ASSERT_EQ(0, photon::fs::resize_extfs(file, 128 * MB));

    extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    struct statvfs st;
    ASSERT_EQ(0, extfs->statvfs("/", &st));
    EXPECT_LE(st.f_blocks * st.f_bsize, 128 * MB);
    for (int i = 0; i < 15; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/data/fill_%03d", i);
        ASSERT_EQ(0, verify_data_file(extfs, path, 4 * MB));
    }
    delete extfs;

    delete file;
}

TEST_F(ResizeTest, ShrinkTooSmall) {
    auto file = create_image(256);
    ASSERT_NE(nullptr, file);

    auto extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    ASSERT_EQ(0, write_data_file(extfs, "/bigdata", 100 * MB));
    delete extfs;

    EXPECT_NE(0, photon::fs::resize_extfs(file, 64 * MB));

    extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    ASSERT_EQ(0, verify_data_file(extfs, "/bigdata", 100 * MB));
    delete extfs;

    delete file;
}

TEST_F(ResizeTest, AllGroupsOccupied) {
    auto file = create_image(256);
    ASSERT_NE(nullptr, file);

    auto extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    extfs->mkdir("/data", 0755);
    int nfiles = 0;
    for (uint64_t offset_mb = 0; offset_mb < 240; offset_mb += 32) {
        char path[64];
        snprintf(path, sizeof(path), "/data/file_%03d", nfiles);
        auto f = extfs->open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        ASSERT_NE(nullptr, f);
        std::string chunk(4 * MB, 'A' + (nfiles % 26));
        ASSERT_EQ((ssize_t)chunk.size(), f->pwrite(chunk.data(), chunk.size(), 0));
        delete f;
        nfiles++;
    }
    delete extfs;

    ASSERT_EQ(0, photon::fs::resize_extfs(file, 200 * MB));

    extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    for (int i = 0; i < nfiles; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/data/file_%03d", i);
        auto f = extfs->open(path, O_RDONLY, 0);
        ASSERT_NE(nullptr, f);
        char buf[32];
        auto ret = f->pread(buf, 32, 0);
        EXPECT_EQ(32, ret);
        if (ret == 32) {
            char expected = 'A' + (i % 26);
            EXPECT_EQ(expected, buf[0]);
        }
        delete f;
    }
    struct statvfs st;
    ASSERT_EQ(0, extfs->statvfs("/", &st));
    EXPECT_LE(st.f_blocks * st.f_bsize, 200 * MB);
    delete extfs;

    file->ftruncate(512 * MB);
    ASSERT_EQ(0, photon::fs::resize_extfs(file, 512 * MB));

    extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    ASSERT_EQ(0, write_data_file(extfs, "/data/after_expand", 1 * MB));
    ASSERT_EQ(0, verify_data_file(extfs, "/data/after_expand", 1 * MB));
    delete extfs;

    delete file;
}

TEST_F(ResizeTest, ShrinkToMinimum) {
    auto file = create_image(256);
    ASSERT_NE(nullptr, file);

    auto extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    ASSERT_EQ(0, write_data_file(extfs, "/data", 10 * MB));
    delete extfs;

    ASSERT_EQ(0, photon::fs::resize_extfs(file, 128 * MB));

    extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    ASSERT_EQ(0, verify_data_file(extfs, "/data", 10 * MB));
    struct statvfs st;
    ASSERT_EQ(0, extfs->statvfs("/", &st));
    EXPECT_LE(st.f_blocks * st.f_bsize, 128 * MB);
    delete extfs;

    delete file;
}

TEST_F(ResizeTest, ExpandThenWrite) {
    auto file = create_image(128);
    ASSERT_NE(nullptr, file);

    file->ftruncate(512 * MB);
    ASSERT_EQ(0, photon::fs::resize_extfs(file, 512 * MB));

    auto extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    ASSERT_EQ(0, write_data_file(extfs, "/new_space_data", 200 * MB));
    ASSERT_EQ(0, verify_data_file(extfs, "/new_space_data", 200 * MB));
    struct statvfs st;
    ASSERT_EQ(0, extfs->statvfs("/", &st));
    EXPECT_GE(st.f_blocks * st.f_bsize, 400 * MB);
    delete extfs;

    delete file;
}

TEST_F(ResizeTest, MultipleResizes) {
    auto file = create_image(512);
    ASSERT_NE(nullptr, file);

    auto extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    ASSERT_EQ(0, write_data_file(extfs, "/persistent", 4 * MB));
    delete extfs;

    ASSERT_EQ(0, photon::fs::resize_extfs(file, 256 * MB));
    file->ftruncate(1024 * MB);
    ASSERT_EQ(0, photon::fs::resize_extfs(file, 1024 * MB));
    ASSERT_EQ(0, photon::fs::resize_extfs(file, 128 * MB));
    file->ftruncate(512 * MB);
    ASSERT_EQ(0, photon::fs::resize_extfs(file, 512 * MB));

    extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    ASSERT_EQ(0, verify_data_file(extfs, "/persistent", 4 * MB));
    delete extfs;

    delete file;
}

TEST_F(ResizeTest, EmptyShrinkToMinimum) {
    auto file = create_image(512);
    ASSERT_NE(nullptr, file);

    ASSERT_EQ(0, photon::fs::resize_extfs(file, 128 * MB));

    auto extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    struct statvfs st;
    ASSERT_EQ(0, extfs->statvfs("/", &st));
    EXPECT_LE(st.f_blocks * st.f_bsize, 128 * MB);
    ASSERT_EQ(0, write_data_file(extfs, "/after_shrink", 1 * MB));
    ASSERT_EQ(0, verify_data_file(extfs, "/after_shrink", 1 * MB));
    delete extfs;

    delete file;
}

TEST_F(ResizeTest, ShrinkReducesGroups) {
    auto file = create_image(512);
    ASSERT_NE(nullptr, file);

    auto extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    extfs->mkdir("/data", 0755);
    for (int i = 0; i < 20; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/data/file_%03d", i);
        ASSERT_EQ(0, write_data_file(extfs, path, 1 * MB));
    }
    delete extfs;

    ASSERT_EQ(0, photon::fs::resize_extfs(file, 128 * MB));

    extfs = photon::fs::new_extfs(file);
    ASSERT_NE(nullptr, extfs);
    for (int i = 0; i < 20; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/data/file_%03d", i);
        ASSERT_EQ(0, verify_data_file(extfs, path, 1 * MB));
    }
    struct statvfs st;
    ASSERT_EQ(0, extfs->statvfs("/", &st));
    EXPECT_LE(st.f_blocks * st.f_bsize, 128 * MB);
    delete extfs;

    delete file;
}

int main(int argc, char **argv) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    set_log_output_level(1);
    ::testing::InitGoogleTest(&argc, argv);
    auto ret = RUN_ALL_TESTS();
    photon::fini();
    return ret;
}
