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
#include <thread>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>
#include <malloc.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include <random>

#define protected public
#define private public

#include "../subfs.cpp"
#include "../../iovector.cpp"
#include "../localfs.cpp"
#include "../virtual-file.cpp"
#include "../path.cpp"
#include "../aligned-file.cpp"
#include "../../enumerable.h"
#include "../path.h"
#include "../localfs.h"
#include "../range-split.h"
#include "../range-split-vi.h"
#include "../filesystem.h"
#include "../aligned-file.h"
#include "../../utility.h"
#include "../../alog.h"
#include "../../photon/thread11.h"
#include "../../photon/syncio/fd-events.h"

#include "mock.h"

using namespace std;
using namespace FileSystem;

TEST(Path, split)
{
    static const char* paths[]={
        "/asdf/jkl/bmp/qwer/x.jpg",
        "/kqw/wek///kjas/nn",
        "asdf",
        "/",
        "/qwer/jkl/",
        "/asdf",
    };

    vector<vector<string>> std_results = {
        {"asdf", "jkl", "bmp", "qwer", "x.jpg"},
        {"kqw", "wek", "kjas", "nn"},
        {"asdf"},
        {},
        {"qwer", "jkl"},
        {"asdf"},
    };

    vector<vector<string>> std_dir_results = {
        {"asdf", "jkl", "bmp", "qwer"},
        {"kqw", "wek", "kjas"},
        {},
        {},
        {"qwer", "jkl"},
        {},
    };

    auto it = std_results.begin();
    auto itd = std_dir_results.begin();
    for (auto p: paths)
    {
        Path path(p);
        vector<string> sp;
        for (auto x: path)
            sp.push_back(x.to_string());
        EXPECT_EQ(sp, *it);
        ++it;

        sp.clear();
        for (auto x: path.directory())
            sp.push_back(x.to_string());
        EXPECT_EQ(sp, *itd);
        ++itd;

        for (auto it = path.begin(); it!=path.end(); ++it)
            cout << it->to_string() << ", ";
        cout << endl;
    }
}

TEST(Path, xnames)
{
    static const char* paths[][3]={
        {"/asdf/jkl/bmp/qwer/x.jpg", "/asdf/jkl/bmp/qwer/", "x.jpg"},
        {"/x.jpg", "/", "x.jpg"},
        {"x.jpg", "", "x.jpg"},
        {"/kqw/wek///kjas/nn", "/kqw/wek///kjas/", "nn"},
        {"/kqw/wek///kjas/nn/", "/kqw/wek///kjas/", "nn"},
        {"/kqw/wek///kjas/nn///", "/kqw/wek///kjas/", "nn"},
    };
    for (auto p: paths)
    {
        Path path(p[0]);
        EXPECT_EQ(path.dirname(), p[1]);
        EXPECT_EQ(path.basename(), p[2]);
    }
}

TEST(Path, level_valid_ness)
{
    static pair<const char*, bool> cases[] = {
        {"/asdf/jkl/bmp/qwer/x.jpg", true},
        {"/x.jpg/../../x.jpg", false},
        {"asdf/../../x.jpg", false},
        {"../asdf", false},
    };
    for (auto& c: cases)
    {
        EXPECT_EQ(path_level_valid(c.first), c.second);
    }
}

TEST(string_view, equality) // dependented by `Path`
{
    FileSystem::string_view a(nullptr, 0UL);
    FileSystem::string_view b((char*)234, 0UL);
    EXPECT_EQ(a, b);
}

TEST(Tree, node)
{
    static const char* items[] = {"asdf", "jkl", "qwer", "zxcv", };
    static const char* subnodes[] = {">asdf", ">jkl", ">qwer", ">zxcv", };
    FileSystem::string_view k1234 = "1234";
    auto v1234 = (void*)23456;
    uint64_t F = 2314, f;
    Tree::Node node;

    f = F;
    for (auto x: items)
        node.creat(x, (void*)f++);
    node.creat(k1234, (void*)1234);
    node.creat(k1234, (void*)2345);
    node.creat(k1234, v1234);
    EXPECT_EQ(node.size(), 5);

    for (auto x: subnodes)
        node.mkdir(x);

    EXPECT_EQ(node.size(), 9);

    f = F;
    void* v;
    for (auto x: items)
    {
        node.read(x, &v);
        EXPECT_EQ(v, (void*)f++);
        node.unlink(x);
        EXPECT_FALSE(node.is_file(x));
    }

    for (auto x: subnodes)
    {
        EXPECT_TRUE(node.is_dir(x));
        node.rmdir(x);
        EXPECT_FALSE(node.is_dir(x));
    }

    node.read(k1234, &v);
    EXPECT_EQ(v, (void*)1234);
}

void (*old_free)(void *ptr, const void *caller);
void *(*old_malloc)(size_t size, const void *caller);

void *my_malloc(size_t size, const void *caller);
void my_free(void *ptr, const void *caller);

void malloc_hook() {
    __malloc_hook = my_malloc;
    __free_hook = my_free;
}

void malloc_unhook() {
    __malloc_hook = old_malloc;
    __free_hook = old_free;
}

void init_hook() {
    old_malloc = __malloc_hook;
    old_free = __free_hook;
}

void *my_malloc(size_t size, const void *caller) {
    return nullptr;
}

void my_free(void *ptr, const void *caller) {
    malloc_unhook();
    free(ptr);
}

TEST(range_split, sub_range)
{
    // EXPECT_FALSE(FileSystem::sub_range());
    auto sr = FileSystem::sub_range(0, 0, 0);
    EXPECT_FALSE(sr);
    sr.assign(0, 233, 1024);
    EXPECT_TRUE(sr);
    EXPECT_EQ(233, sr.begin());
    EXPECT_EQ(233+1024, sr.end());
    sr.clear();
    EXPECT_FALSE(sr);
    sr.assign(1, 233, 1024);
}

TEST(range_split, range_split_simple_case)
{
    FileSystem::range_split split(42, 321, 32); // offset 42, length 321, interval 32
    // it should be split into [begin, end) as [42, 64)+[64, 76) +... +[352,363)
    // with abegin, aend as 0, 11
    // 11 parts in total
    EXPECT_FALSE(split.small_note);
    EXPECT_EQ(42, split.begin);
    EXPECT_EQ(363, split.end);
    EXPECT_EQ(1, split.abegin);
    EXPECT_EQ(12, split.aend);
    EXPECT_EQ(2, split.apbegin);
    EXPECT_EQ(split.apend,11);
    EXPECT_EQ(32, split.aligned_begin_offset());
    EXPECT_EQ(384, split.aligned_end_offset());
    auto p = split.all_parts();
    EXPECT_EQ(1, p.begin()->i);
    EXPECT_EQ(10, p.begin()->begin());
    EXPECT_EQ(32, p.begin()->end());
    EXPECT_EQ(12, p.end()->i);
    int cnt = 1;
    for (auto &rs: p) {
        EXPECT_EQ(cnt++, rs.i);
        if (rs != p.begin() && rs != p.end()) {
            EXPECT_EQ(0, rs.begin());
            EXPECT_EQ(32, rs.end());
        }
    }
    split = FileSystem::range_split(2, 12, 24);
    EXPECT_TRUE(split.small_note);
    EXPECT_FALSE(split.preface);
    EXPECT_FALSE(split.postface);
    for (auto it = p.begin();it != p.end(); ++it) {
        EXPECT_EQ(it, it);
    }
}

TEST(range_split, range_split_aligned_case)
{
    FileSystem::range_split split(32, 321, 32); // offset 32, length 321, interval 32
    // it should be split into [begin, end) as [32, 64)+[64, 76) +... +[352,353)
    // with abegin, aend as 0, 11
    // 11 parts in total
    EXPECT_EQ(32, split.begin);
    EXPECT_EQ(353, split.end);
    EXPECT_EQ(1, split.abegin);
    EXPECT_EQ(12, split.aend);
    EXPECT_EQ(1, split.apbegin);
    EXPECT_EQ(11, split.apend);
    auto p = split.all_parts();
    EXPECT_FALSE(split.is_aligned());
    EXPECT_TRUE(split.is_aligned(128));
    EXPECT_TRUE(split.is_aligned_ptr((const void*)(uint64_t(65536))));
    EXPECT_EQ(1, p.begin()->i);
    EXPECT_EQ(0, p.begin()->begin());
    EXPECT_EQ(32, p.begin()->end());
    EXPECT_EQ(12, p.end()->i);
    EXPECT_EQ(352, split.aligned_length());
    auto q = split.aligned_parts();
    int cnt = 1;
    for (auto &rs: q) {
        EXPECT_EQ(cnt++, rs.i);
        EXPECT_EQ(0, rs.begin());
        EXPECT_EQ(32, rs.end());
    }
    split = FileSystem::range_split(0, 23, 24);
    EXPECT_TRUE(split.postface);
    split = FileSystem::range_split(1, 23, 24);
    EXPECT_TRUE(split.preface);
    split = FileSystem::range_split(0, 24, 24);
    EXPECT_FALSE(split.preface);
    EXPECT_FALSE(split.postface);
    EXPECT_FALSE(split.small_note);
    EXPECT_TRUE(split.aligned_parts().begin()->i + 1 == split.aligned_parts().end().i);
}

TEST(range_split, random_test) {
    uint64_t begin=rand(), length=rand(), interval=rand();
    LOG_DEBUG("begin=", begin, " length=", length, " interval=", interval);
    FileSystem::range_split split(begin, length, interval);
    EXPECT_EQ(split.begin, begin);
    EXPECT_EQ(split.end, length + begin);
    EXPECT_EQ(split.interval, interval);
}


TEST(range_split_power2, basic) {
    FileSystem::range_split_power2 split(42, 321, 32);
    for (auto &rs: split.all_parts()) {
        LOG_DEBUG(rs.i, ' ', rs.begin(), ' ', rs.end());
    }
    EXPECT_FALSE(split.small_note);
    EXPECT_EQ(42, split.begin);
    EXPECT_EQ(363, split.end);
    EXPECT_EQ(1, split.abegin);
    EXPECT_EQ(12, split.aend);
    EXPECT_EQ(2, split.apbegin);
    EXPECT_EQ(11, split.apend);
    EXPECT_EQ(32, split.aligned_begin_offset());
    EXPECT_EQ(384, split.aligned_end_offset());
    auto p = split.all_parts();
    EXPECT_EQ(p.begin()->i, 1);
    EXPECT_EQ(p.begin()->begin(), 10);
    EXPECT_EQ(p.begin()->end(), 32);
    EXPECT_EQ(p.end()->i, 12);
    int cnt = 1;
    for (auto &rs: p) {
        EXPECT_EQ(rs.i, cnt++);
        if (rs != p.begin() && rs != p.end()) {
            EXPECT_EQ(rs.begin(), 0);
            EXPECT_EQ(rs.end(), 32);
        }
    }
}

TEST(range_split_power2, random_test) {
    uint64_t offset, length, interval;
    offset = rand();
    length = rand();
    auto interval_shift = rand()%32 + 1;
    interval = 1<<interval_shift;
    FileSystem::range_split_power2 split(offset, length, interval);
    EXPECT_EQ(offset, split.begin);
    EXPECT_EQ(offset + length, split.end);
    EXPECT_EQ(interval, split.interval);
}

void not_ascend_death()
{
    uint64_t kpfail[] = {0, 32, 796, 128, 256, 512, UINT64_MAX};
    FileSystem::range_split_vi splitfail(12, 321, kpfail, 7);
}
TEST(range_split_vi, basic) {
    uint64_t kp[] = {0, 32, 64, 128, 256, 512, UINT64_MAX};
    FileSystem::range_split_vi split(12, 321, kp, 7);
    uint64_t *it = kp;
    EXPECT_EQ(12, split.begin);
    EXPECT_EQ(333, split.end);
    EXPECT_TRUE(split.is_aligned((uint64_t)0));
    EXPECT_FALSE(split.is_aligned(1));
    EXPECT_TRUE(split.is_aligned(128));
    for (auto p : split.all_parts()) {
        LOG_DEBUG(p.i, ' ', p.begin(), ' ', p.end());
        EXPECT_EQ(*it == 0?12:0, p.begin());
        EXPECT_EQ(*it == 256? 321-256+12 :(*(it+1) - *it), p.end());
        it++;
    }
    uint64_t kpfail[] = {0, 32, 796, 128, 256, 512, UINT64_MAX};
    EXPECT_FALSE(split.ascending(kpfail, 7));
    EXPECT_DEATH(
        not_ascend_death(),
        ".*FileSystem::range_split_vi::range_split_vi.*"
    );
}

TEST(range_split_vi, left_side_aligned) {
    uint64_t kp[] = {0, 32, 64, 128, 256, 512, UINT64_MAX};
    FileSystem::range_split_vi split(0, 256, kp, 7);
    uint64_t *it = kp;
    EXPECT_EQ(0, split.begin);
    EXPECT_EQ(256, split.end);
    EXPECT_TRUE(split.is_aligned((uint64_t)0));
    EXPECT_FALSE(split.is_aligned(1));
    EXPECT_TRUE(split.is_aligned(128));
    for (auto p : split.all_parts()) {
        LOG_DEBUG(p.i, ' ', p.begin(), ' ', p.end());
        EXPECT_EQ(0, p.begin());
        EXPECT_EQ((*(it+1) - *it), p.end());
        it++;
    }
}

TEST(LocalFileSystem, basic) {
    using namespace FileSystem;
    std::unique_ptr<IFileSystem> fs(new_localfs_adaptor("/tmp/"));
    std::unique_ptr<IFile> lf(fs->open("test_local_fs", O_RDWR | O_CREAT, 0755));
    DEFER(lf->close(););
    lf->pwrite("HELLO", 5, 0);
    lf->fsync();
}

std::unique_ptr<char[]> random_block(uint64_t size) {
    std::unique_ptr<char[]> buff(new char[size]);
    char * p = buff.get();
    while (size--) {
        *(p++) = rand() % UCHAR_MAX;
    }
    return std::move(buff);
}

void random_content_rw_test(uint64_t test_block_size, uint64_t test_block_num, FileSystem::IFile* file) {
    vector<std::unique_ptr<char[]>> rand_data;
    file->lseek(0, SEEK_SET);
    for (auto i = 0; i< test_block_num; i++) {
        rand_data.emplace_back(std::move(random_block(test_block_size)));
        char * buff = rand_data.back().get();
        file->write(buff, test_block_size);
    }
    file->fsync();
    file->lseek(0, SEEK_SET);
    char buff[test_block_size];
    for (const auto &data : rand_data) {
        file->read(buff, test_block_size);
        EXPECT_EQ(0, memcmp(data.get(), buff, test_block_size));
    }
}

void sequence_content_rw_test (uint64_t test_block_size, uint64_t test_block_num, const char* test_seq, FileSystem::IFile* file) {
    char data[test_block_size];
    file->lseek(0, SEEK_SET);
    for (auto i = 0; i< test_block_num; i++) {
        memset(data, test_seq[i], test_block_size);
        file->write(data, test_block_size);
    }
    file->fdatasync();
    file->lseek(0, SEEK_SET);
    char buff[test_block_size];
    for (auto i = 0; i< test_block_num; i++) {
        file->read(buff, test_block_size);
        memset(data, *(test_seq++), test_block_size);
        EXPECT_EQ(0, memcmp(data, buff, test_block_size));
    }
}

void xfile_fstat_test(uint64_t fsize, FileSystem::IFile* file) {
    struct stat st;
    file->fstat(&st);
    EXPECT_EQ(fsize, st.st_size);
}

void xfile_not_impl_test(FileSystem::IFile* file) {
    auto retp = file->filesystem();
    EXPECT_EQ(nullptr, retp);
    EXPECT_EQ(ENOSYS, errno);
    auto ret = file->ftruncate(1024);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ENOSYS, errno);
    ret = file->fchmod(0755);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ENOSYS, errno);
    ret = file->fchown(0, 0);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ENOSYS, errno);
    errno = 0;
}

void fill_random_buff(char * buff, size_t length) {
    for (size_t i = 0; i< length; i++) {
        buff[i] = rand() % UCHAR_MAX;
    }
}

void pread_pwrite_test(IFile *target, IFile *standard) {
    constexpr int max_file_size = 65536;
    constexpr int max_piece_length = 16384;
    constexpr int test_round = 10;
    char data[max_piece_length];
    char buff[max_piece_length];
    for (int i = 0;i < test_round; i++) {
        off_t off = rand() % max_file_size / getpagesize() * getpagesize();
        size_t len = rand() % max_piece_length / getpagesize() * getpagesize();
        if (off+len > max_file_size) {
            continue;
        }
        fill_random_buff(buff, len);
        target->pwrite(buff, len, off);
        standard->pwrite(buff, len, off);
        target->pread(data, len, off);
        EXPECT_EQ(0, memcmp(data, buff, len));
    }
    for (off_t off = 0; off < max_file_size; off+=max_piece_length) {
        auto len = target->pread(buff, max_piece_length, off);
        auto ret = standard -> pread(data, max_piece_length, off);
        EXPECT_EQ(len, ret);
        EXPECT_EQ(0, memcmp(data, buff, ret));
    }
    for (int i = 0;i < test_round; i++) {
        off_t off = rand() % max_file_size;
        size_t len = rand() % max_piece_length;
        if (off+len > max_file_size) {
            len = max_file_size - off;
        }
        fill_random_buff(buff, len);
        target->pwrite(buff, len, off);
        standard->pwrite(buff, len, off);
        target->pread(data, len, off);
        EXPECT_EQ(0, memcmp(data, buff, len));
    }
    for (off_t off = 0; off < max_file_size; off+=max_piece_length) {
        target->pread(buff, max_piece_length, off);
        standard -> pread(data, max_piece_length, off);
        EXPECT_EQ(0, memcmp(data, buff, max_piece_length));
    }
    struct stat stat;
    target->fstat(&stat);
    auto tsize = stat.st_size;
    standard->fstat(&stat);
    auto rsize = stat.st_size;
    EXPECT_EQ(rsize, tsize);
}

TEST(AlignedFileAdaptor, basic) {
    IFileSystem *fs = new_localfs_adaptor("/tmp/");
    IFile *normal_file = fs->open("test_aligned_file_normal", O_RDWR | O_CREAT, 0666);
    normal_file->ftruncate(65536);
    DEFER(normal_file->close());
    IFile *underlay_file = fs->open("test_aligned_file_aligned", O_RDWR | O_CREAT, 0666);
    underlay_file->ftruncate(65536);
    std::unique_ptr<IFile> aligned_file(new_aligned_file_adaptor(underlay_file, getpagesize(), true, false));
    DEFER(aligned_file->close());
    pread_pwrite_test(aligned_file.get(), normal_file);
    std::unique_ptr<IFile> aligned_file_2(new_aligned_file_adaptor(underlay_file, getpagesize(), false, true));
    DEFER(aligned_file_2->close());
    pread_pwrite_test(aligned_file_2.get(), normal_file);
}

TEST(AlignedFileAdaptor, err_situation) {
    log_output = log_output_null;
    DEFER({
        log_output = log_output_stdout;
    });
    using namespace testing;
    IFileSystem *fs = new_localfs_adaptor("/tmp/");
    IFile *underlay_file = fs->open("test_aligned_file_aligned", O_RDWR | O_CREAT, 0666);
    underlay_file->ftruncate(65536);
    IFile *wrong_size_aligned_file = new_aligned_file_adaptor(underlay_file, getpagesize() - 1, true, false);
    EXPECT_EQ(nullptr, wrong_size_aligned_file);
    EXPECT_EQ(EINVAL, errno);
    Mock::MockNullFile mock_file;
    EXPECT_CALL(mock_file, pread(_, _, _))
        .Times(AtLeast(1))
        .WillRepeatedly(Return(-1));
    EXPECT_CALL(mock_file, fstat(_))
        .Times(AtLeast(1))
        .WillRepeatedly(Return(-1));
    IFile *aligned_unreadable = new_aligned_file_adaptor(&mock_file, getpagesize(), true, false);
    char buff[4096];
    auto ret = aligned_unreadable->pread(buff, 4096, 0);
    EXPECT_EQ(-1, ret);
    ret = aligned_unreadable->pwrite(buff, 128, 1);
    EXPECT_EQ(-1, ret);
    ret = aligned_unreadable->pwrite(buff, 128, 1);
    EXPECT_EQ(-1, ret);
}

TEST(range_split_vi, special_case) {
    auto offset = 10601376;
    auto len = 2256;
    vector<uint64_t> kp{0, offset, offset+len, UINT64_MAX};
    range_split split(10601376, 2256, 4096);
    ASSERT_TRUE(split.small_note);
    auto cnt = 0;
    for (auto &part : split.aligned_parts()) {
        auto iovsplit = range_split_vi(
                split.multiply(part.i, 0),
                part.length,
                &kp[0],
                kp.size()
        );
        cnt++;
        ASSERT_LT(1, cnt);
    }
}

TEST(AlignedFile, pwrite_at_tail) {
    IFileSystem *fs = new_localfs_adaptor();
    IFileSystem *afs = new_aligned_fs_adaptor(fs, 4096, true, true);
    IFile *file = afs->open("/tmp/obd_aligned_file_test.data", O_CREAT | O_TRUNC | O_RDWR, 0644);
    auto ret = file->pwrite("wtf", 3, 0);
    EXPECT_EQ(3, ret);
    delete file;
    struct stat stat;
    afs->stat("/tmp/obd_aligned_file_test.data", &stat);
    EXPECT_EQ(3, stat.st_size);
    delete afs;
}

inline static void SetupTestDir(const std::string& dir) {
  std::string cmd = std::string("rm -r ") + dir;
  system(cmd.c_str());
  cmd = std::string("mkdir -p ") + dir;
  system(cmd.c_str());
}

TEST(Walker, basic) {
  std::string root("/tmp/obdtest_walker");
  SetupTestDir(root);
  auto srcFs = new_localfs_adaptor(root.c_str(), ioengine_psync);
  DEFER(delete srcFs);

  for (auto file : enumerable(Walker(srcFs, ""))) {
    EXPECT_FALSE(true);
  }

  std::string file1("/testFile");
  std::system(std::string("touch " + root + file1).c_str());
  for (auto file : enumerable(Walker(srcFs, ""))) {
    EXPECT_EQ(0, strcmp(file.data(), file1.c_str()));
  }
  for (auto file : enumerable(Walker(srcFs, "/"))) {
    EXPECT_EQ(0, strcmp(file.data(), file1.c_str()));
  }

  std::string file2("/dir1/dir2/dir3/dir4/dirFile2");
  std::system(std::string("mkdir -p " + root + "/dir1/dir2/dir3/dir4/").c_str());
  std::system(std::string("touch " + root + file2).c_str());
  int count = 0;
  for (auto file : enumerable(Walker(srcFs, "/"))) {
    if (file.back() == '2') {
      EXPECT_EQ(0, strcmp(file.data(), file2.c_str()));
    } else {
      EXPECT_EQ(0, strcmp(file.data(), file1.c_str()));
    }
    count++;
  }
  EXPECT_EQ(2, count);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    photon::init();
    photon::fd_events_init();
    DEFER({
        photon::fd_events_fini();
        photon::fini();
    });
    int ret = RUN_ALL_TESTS();
    LOG_ERROR_RETURN(0, ret, VALUE(ret));
}
