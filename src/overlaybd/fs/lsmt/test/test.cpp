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

/*
VirualReadOnly -> IFileRO -> IFileRW -> LSMTReadOnlyFile -> LSMTFile

IMemoryIndex -> IMemoryIndex0 -> IComboIndex -> Index0 ( set<SegmentMap> ) -> ComboIndex
         |
         | -> Index ( vector<SegmentMap> )

*/
#include "lsmt-filetest.h"
#include <sys/time.h>
#include "../../../photon/syncio/fd-events.h"
#include "../../../photon/syncio/aio-wrapper.h"

#define USE_PTH true // use pthread

//#define TEST_RANGE 1024*512

namespace LSMT {
bool operator==(SegmentMapping a, SegmentMapping b) {
    return memcmp(&a, &b, sizeof(a)) == 0;
}
} // namespace LSMT

template <typename IDX>
void lookup_test(const SegmentMapping *mapping, size_t n1, Segment s, const SegmentMapping *stdrst,
                 size_t n2) {
    SegmentMapping out[10];
    IDX idx(mapping, n1, false);
    size_t n = idx.lookup(s, out, LEN(out));

    EXPECT_EQ(n, n2);
    for (size_t i = 0; i < min(n, n2); ++i) {
        cout << i << endl;
        EXPECT_EQ(out[i], stdrst[i]);
    }
}

template <typename IDX, size_t N1, size_t N2>
void lookup_test(const SegmentMapping (&mapping)[N1], Segment s,
                 const SegmentMapping (&stdrst)[N2]) {
    lookup_test<IDX>(mapping, N1, s, stdrst, N2);
}

void lookup_test(IMemoryIndex &idx);

TEST(Index, lookup) {
    const static SegmentMapping mapping[] = {{0, 10, 0}, {10, 10, 50}, {100, 10, 20}};
    Index idx(mapping, LEN(mapping), false);

    lookup_test<Index>(mapping, {5, 10}, {{5, 5, 5}, {10, 5, 50}});
    lookup_test<Index>(mapping, {16, 10}, {{16, 4, 56}});
    lookup_test<Index>(mapping, LEN(mapping), {26, 10}, nullptr, 0);
    lookup_test<Index>(mapping, {6, 100}, {{6, 4, 6}, {10, 10, 50}, {100, 6, 20}});

    LevelIndex idx1(mapping, LEN(mapping), false);
    lookup_test<LevelIndex>(mapping, {5, 10}, {{5, 5, 5}, {10, 5, 50}});
    lookup_test<LevelIndex>(mapping, {16, 10}, {{16, 4, 56}});
    lookup_test<LevelIndex>(mapping, LEN(mapping), {26, 10}, nullptr, 0);
    lookup_test<LevelIndex>(mapping, {6, 100}, {{6, 4, 6}, {10, 10, 50}, {100, 6, 20}});
}

const static SegmentMapping mapping0[] = {{0, 20, 0},    {10, 15, 50},    {30, 100, 20}, {5, 10, 3},
                                          {40, 10, 123}, {200, 10, 2133}, {150, 100, 21}};

TEST(Index0, insert) {

    Index0 idx(mapping0, LEN(mapping0));

    auto p = idx.dump();
    for (size_t i = 0; i < idx.size() - 1; ++i)
        EXPECT_TRUE(p[i].end() <= p[i + 1].offset);

    const static SegmentMapping stdrst[] = {
        {0, 5, 0},     {5, 10, 3},   {15, 10, 55},   {30, 10, 20},
        {40, 10, 123}, {50, 80, 40}, {150, 100, 21},
    };
    EXPECT_EQ(idx.size(), LEN(stdrst));
    for (size_t i = 0; i < min(idx.size(), LEN(stdrst)); ++i) {
        cout << i << endl;
        EXPECT_EQ(stdrst[i], p[i]);
    }
    uint64_t check = 0;
    for (size_t i = 0; i < idx.size(); ++i) {
        check += p[i].length * (!p[i].zeroed);
    }
    delete[] p;
    ASSERT_EQ(check, idx.block_count());
}


#define RAND_RANGE                                                                                 \
    (uint64_t)(rand() % ((32 << 20) - 128)),                                                       \
        (uint32_t)(rand() % (1 << 6) + 1)

uint64_t max_offset = 0;
static void do_randwrite(/* LSMT:: */ IComboIndex *idx0, uint32_t *moffsets) {
    SegmentMapping s{RAND_RANGE, /*moffset = */ (uint64_t)(rand() % 10000000 + 1)};
    if (max_offset < s.offset)
        max_offset = s.offset;
    idx0->insert(s);
    auto x = s.moffset;
    for (auto j = s.offset; j < s.end(); ++j)
        moffsets[j] = x++;
}

static void do_randread(/* LSMT:: */ IMemoryIndex *mi, uint32_t *moffsets) {
    auto s = Segment{RAND_RANGE};
    if (s.offset > max_offset)
        s.offset = max_offset;
    foreach_segments(
        mi, s,
        [&](const Segment &m) __attribute__((always_inline)) {
            for (auto x : ptr_array(moffsets + m.offset, m.length))
                EXPECT_EQ(x, (unsigned)0);
            return 0;
        },
        [&](const SegmentMapping &m) __attribute__((always_inline)) {
            auto delta = 0;
            for (auto x : ptr_array(moffsets + m.offset, m.length)) {
                EXPECT_EQ(x, m.moffset + delta);
                delta++;
            }
            return 0;
        });
}

TEST(Layered, Indexes) {

    const size_t MaxLayers = FLAGS_layers;
    const IMemoryIndex *layers[MaxLayers];
    layers[MaxLayers - 1] = create_level_index(nullptr, 0, 0, UINT64_MAX, false);

    static uint32_t moffsets[32 << 20];
    memset(moffsets, 0, sizeof(moffsets));

    printf("# of layers: ");
    for (size_t k = 1; k < MaxLayers; ++k) {
        printf("%d\n", (int)k);
        fflush(stdout);
        auto idx0 = (Index0 *)create_memory_index0();
        auto mi = merge_memory_indexes(&layers[MaxLayers - k], k);
        auto ci = create_combo_index(idx0, mi, false);
        EXPECT_EQ(idx0->backing_index(mi), -1);
        EXPECT_EQ(idx0->increase_tag(1), -1);
        EXPECT_EQ(idx0->load_range_index(0, 1000), nullptr);
        EXPECT_EQ(idx0->backing_index(), nullptr);

        for (int i = 0; i < FLAGS_nwrites; ++i) {
            // printf("%d\n",i);
            do_randwrite(ci, moffsets);
        }

        //   printf("randread");
        for (int i = 0; i < FLAGS_nwrites / 2; ++i)
            do_randread(ci, moffsets);
        auto p = ci->dump();
        auto ri = ci->load_range_index(0, 100);
        if (ri) {
            auto ridx = ptr_array(ri->buffer(), ri->size());
            auto backing_idx = ci->backing_index();
            auto mdump = backing_idx->buffer();
            EXPECT_EQ(ri->size(), backing_idx->size());
            int i = 0;
            for (auto idx : ridx) {
                EXPECT_EQ(idx.offset, mdump[i].offset);
                i++;
            }
            LOG_INFO(backing_idx->front());
            LOG_INFO(backing_idx->back());
        }
        auto idx = create_level_index(p, ci->size(), 0, UINT64_MAX, false);
        delete ci;
        delete mi;
        layers[MaxLayers - k - 1] = idx;
        delete idx0;
    }
    printf("\n");
}

IMemoryIndex0 *idx0 = create_memory_index0();

TEST(Perf, Index0_randwrite1M) {

    for (int i = 0; i < 1000000; ++i)
        idx0->insert({RAND_RANGE, (uint64_t)i});
    cout << idx0->size() << " elements in the index" << endl;
    auto p = idx0->dump();
    size_t index_size = 0;
    for (size_t i = 0; i < idx0->size(); i++) {
        index_size += p[i].length * (!p[i].zeroed);
    }
    ASSERT_EQ(index_size, idx0->block_count());
}

void test_randread1M(IMemoryIndex *idx) {
    // SegmentMapping pm[10];
    for (int i = 0; i < 1000 * 1000; ++i) {
        auto s = Segment{RAND_RANGE};
        foreach_segments(
            idx, s, [&](const Segment &m) __attribute__((always_inline)) { return 0; },
            [&](const SegmentMapping &m) __attribute__((always_inline)) { return 0; });
    }
    cout << idx->size() << endl;
}

TEST(Perf, Index0_randread1M) {
    test_randread1M(idx0);
}

TEST(Perf, Index1_randread1M) {
    auto p = idx0->dump();
    auto idx = create_level_index(p, idx0->size(), 0, UINT64_MAX, false);
    test_randread1M(idx);
    delete idx;
    delete[] p;
}

void test_merge(const IMemoryIndex *indexes[], size_t ni, const SegmentMapping stdrst[],
                size_t nrst) {
    auto mi = merge_memory_indexes(indexes, ni);
    unique_ptr<IMemoryIndex> pmi(mi);
    EXPECT_EQ(mi->size(), nrst);
    if (mi->size() == nrst) {
        auto ret = memcmp(mi->buffer(), stdrst, nrst * sizeof(stdrst[0]));
        EXPECT_EQ(ret, 0);
    }
}

template <size_t NR>
inline void test_merge(const IMemoryIndex *indexes[], size_t ni,
                       const SegmentMapping (&stdrst)[NR]) {
    test_merge(indexes, ni, stdrst, NR);
}

void test_combo(const IMemoryIndex *indexes[], size_t ni, const SegmentMapping stdrst[],
                size_t nrst) {
    auto i0 = create_memory_index0(indexes[0]->buffer(), indexes[0]->size(), 0, 1000000);
    auto mi = merge_memory_indexes(indexes + 1, ni - 1);
    ComboIndex ci((Index0 *)i0, (Index *)mi, true);
    SegmentMapping pm[20];
    assert(LEN(pm) >= nrst);
    auto ret = ci.lookup(Segment{0, 10000}, pm, LEN(pm));
    EXPECT_EQ(ret, nrst);
    if (ret == nrst) {
        auto ret = memcmp(pm, stdrst, nrst * sizeof(stdrst[0]));
        EXPECT_EQ(ret, 0);
    }

    ci.backing_index();
    // delete mi;
    // return;
    auto mi0 = merge_memory_indexes(indexes + 1, ni - 1);
    ci.backing_index(mi0);
}

template <size_t NR>
inline void test_merge_combo(const IMemoryIndex *indexes[], size_t ni, // num of indexes
                             const SegmentMapping (&stdrst)[NR]) {
    test_merge(indexes, ni, stdrst, NR);
    test_combo(indexes, ni, stdrst, NR);
}

TEST(Index, merge) {

    const static SegmentMapping mapping0[] = {{5, 5, 0}, {10, 10, 50}, {100, 10, 20}};
    const static SegmentMapping mapping1[] = {{0, 1, 7},    {2, 4, 5},    {15, 10, 22},
                                              {30, 15, 89}, {87, 50, 32}, {150, 10, 84}};
    const static SegmentMapping mapping2[] = {
        {1, 3, 134}, {8, 4, 873}, {18, 72, 320}, {100, 100, 4893}, {1000, 1000, 39823}};
    const static SegmentMapping mapping3[] = {
        {23, 10, 0}, {65, 10, 50}, {89, 10, 20}, {230, 43, 432}, {1999, 31, 2393}};

    LevelIndex idx0(mapping0, LEN(mapping0), false);
    LevelIndex idx1(mapping1, LEN(mapping1), false);
    LevelIndex idx2(mapping2, LEN(mapping2), false);
    LevelIndex idx3(mapping3, LEN(mapping3), false);
    const IMemoryIndex *indexes[] = {&idx0, &idx1, &idx2, &idx3};

    test_merge_combo(indexes, 2,
                     {{0, 1, 7, 1},
                      {2, 3, 5, 1},
                      {5, 5, 0, 0},
                      {10, 10, 50, 0},
                      {20, 5, 22 + 5, 1},
                      {30, 15, 89, 1},
                      {87, 13, 32, 1},
                      {100, 10, 20, 0},
                      {110, 27, 55, 1},
                      {150, 10, 84, 1}});
    test_merge_combo(indexes, 3,
                     {{0, 1, 7, 1},
                      {1, 1, 134, 2},
                      {2, 3, 5, 1},
                      {5, 5, 0, 0},
                      {10, 10, 50, 0},
                      {20, 5, 22 + 5, 1},
                      {25, 5, 320 + 7, 2},
                      {30, 15, 89, 1},
                      {45, 42, 320 + 27, 2},
                      {87, 13, 32, 1},
                      {100, 10, 20, 0},
                      {110, 27, 55, 1},
                      {137, 13, 4893 + 37, 2},
                      {150, 10, 84, 1},
                      {160, 40, 4893 + 60, 2},
                      {1000, 1000, 39823, 2}});
    test_merge_combo(indexes, 4,
                     {{0, 1, 7, 1},
                      {1, 1, 134, 2},
                      {2, 3, 5, 1},
                      {5, 5, 0, 0},
                      {10, 10, 50, 0},
                      {20, 5, 22 + 5, 1},
                      {25, 5, 320 + 7, 2},
                      {30, 15, 89, 1},
                      {45, 42, 320 + 27, 2},
                      {87, 13, 32, 1},
                      {100, 10, 20, 0},
                      {110, 27, 55, 1},
                      {137, 13, 4893 + 37, 2},
                      {150, 10, 84, 1},
                      {160, 40, 4893 + 60, 2},
                      {230, 43, 432, 3},
                      {1000, 1000, 39823, 2},
                      {2000, 30, 2393 + 1, 3}});
}

void test_compress(SegmentMapping *src, size_t n1, const SegmentMapping *stdrst, size_t n2) {
    auto n1cp = compress_raw_index_predict(src, n1);
    EXPECT_EQ(n1cp, n2);

    auto n1c = compress_raw_index(src, n1);
    EXPECT_EQ(n1c, n2);

    if (n1c == n2) {
        auto ret = memcmp(src, stdrst, n2 * sizeof(*stdrst));
        EXPECT_EQ(ret, 0);
    }
}

template <size_t N1, size_t N2>
void test_compress(const SegmentMapping (&src)[N1], const SegmentMapping (&stdrst)[N2]) {
    test_compress((SegmentMapping *)src, N1, stdrst, N2);
}

TEST(Index, compress) {

    test_compress({{5, 5, 0}, {10, 10, 5}, {100, 10, 20}}, {{5, 15, 0}, {100, 10, 20}});
    test_compress({{5, 5, 0}, {10, 10, 5}, {20, 10, 15}, {100, 10, 20}},
                  {{5, 25, 0}, {100, 10, 20}});
    test_compress({{5, 5, 0}, {10, 10, 5}, {20, 10, 15, 1}, {100, 10, 20}},
                  {{5, 15, 0}, {20, 10, 15, 1}, {100, 10, 20}});
    test_compress({{5, 5, 0}, {10, 10, 5, 3}, {20, 10, 15, 3}, {30, 10, 20}},
                  {{5, 5, 0}, {10, 20, 5, 3}, {30, 10, 20}});
}
// #endif

struct ThreadArgs {
    //    Mutex *mutex;
    int tvar;
    ~ThreadArgs() {
        // concurrency->mutex_destroy(mutex);
    }
};

bool all_0xcc(unsigned char *buf, size_t size) {
    for (auto c : ptr_array(buf, size))
        if (c != 0xcc)
            return false;
    return true;
}

void test_read(IFile *file, uint64_t offset, size_t size) {
    unsigned char buf[1024 * 1024];
    size = min(size, sizeof(buf));
    memset(buf, 0, size);
    auto ret = file->pread(buf, size, offset);
    EXPECT_EQ(ret, (ssize_t)size);
    EXPECT_TRUE(all_0xcc(buf, size));
}

ssize_t file_size(IFileSystem *fs, const char *fn) {
    struct stat stat;
    auto ret = fs->stat(fn, &stat);
    return (ret < 0) ? -1 : stat.st_size;
}

TEST_F(FileTest, create_open) {

    auto file1 = create_file_rw();
    delete file1;
    auto file2 = open_file_rw();
    delete file2;
}

class FileTest1 : public FileTest {
public:
    virtual void SetUp() override {
        char buf[1024];
        memset(buf, 0xcc, sizeof(buf));
        auto file = create_file_rw();
        file->write(buf, sizeof(buf));
        file->close_seal();
        delete file;
    }
};

TEST_F(FileTest2, commit_close_seal) {
    reset_verify_file();
    auto file = create_file();
    cout << "commit()" << endl;
    auto fcommit = lfs->open(layer_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);

    file->commit(fcommit);
    delete fcommit;

    verify_file(layer_name.back().c_str());

    IFileRO *fdup;
    auto index0 = (Index0 *)file->index();
    index0->buffer();
    index0->make_read_only_index();
    cout << "fdup->index.front(): {" << index0->front().offset << ", " << index0->front().length
         << ", " << index0->front().moffset << "}" << endl;
    cout << "file->index.back(): {" << index0->back().offset << ", " << index0->back().length
         << ", " << index0->back().moffset << "}" << endl;
    cout << "close_seal()" << endl;
    off_t offset = DO_ALIGN(rand() % vsize);
    cout << "lower_bound( " << offset
         << " ) == index0->end(): " << (index0->lower_bound(offset) == index0->end()) << endl;

    file->fsync();
    file->fdatasync();
    file->sync_file_range(0, 0, 0);
    file->fchmod(0755);
    file->fchown(0, 0);
    //================================
    cout << "file->index0.size(): " << (file->index())->size() << endl;
    file->close_seal(&fdup);
    auto index = fdup->index();
    cout << "fdup->index0.size(): " << index->size() << endl;
    struct stat fdup_stat;
    fdup->fstat(&fdup_stat);
    cout << "fdup_stat.st_blksize: " << fdup_stat.st_blksize << endl;
    cout << "fdup_stat.st_dev: " << fdup_stat.st_dev << endl;
    /*auto filesys = */ fdup->filesystem();
    cout << "fdup->index.front(): {" << index->front().offset << ", " << index->front().length
         << ", " << index->front().moffset << "}" << endl;
    cout << "fdup->index.back(): {" << index->back().offset << ", " << index->back().length << ", "
         << index->back().moffset << "}" << endl;
    index->buffer();
    // UUID u1 {1,2,3,4}, u2 {1,2,3,4,{2,2,3,4,5,6}};
    UUID u1, u2;
    int t1[4]{1, 2, 3, 4};
    int t2[4]{5, 6, 7, 8};
    u1.reset((char *)(t1), 16);
    u2.reset((char *)(t2), 16);
    cout << "u1 != u2: " << (u1 != u2) << endl;
    delete file;
    verify_file(fdup);
    cout << "end" << endl;
}

TEST_F(FileTest3, stack_files) {
    CleanUp();
    cout << "generating " << FLAGS_layers << " RO layers by randwrite()" << endl;
    for (int i = 0; i < FLAGS_layers; ++i) {
        files[i] = create_commit_layer(0, 1 /*libaio*/);
    }

    cout << "merging RO layers as " << fn_merged << endl;
    auto merged = lfs->open(fn_merged, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    merge_files_ro(files, FLAGS_layers, merged);
    /*auto mergedro =*/::open_file_ro(merged, true);
    cout << "verifying merged RO layers file" << endl;
    cout << "verifying stacked RO layers file" << endl;
    auto lower = open_files_ro(files, FLAGS_layers);
    verify_file(lower);
    ((LSMTReadOnlyFile *)lower)->m_index =
        create_level_index(lower->index()->buffer(), lower->index()->size(), 0, UINT64_MAX, false);
    EXPECT_EQ(((LSMTReadOnlyFile *)lower)->close_seal(), -1);
    CommitArgs _(nullptr);
    EXPECT_EQ(((LSMTReadOnlyFile *)lower)->commit(_), -1);
    auto stat = ((LSMTReadOnlyFile *)lower)->data_stat();
    LOG_INFO("RO valid data: `", stat.valid_data_size);
    cout << "generating a RW layer by randwrite()" << endl;
    auto upper = create_a_layer();

    auto file = stack_files(upper, lower, 0, true);

    verify_file(file);
    delete file;
}

TEST_F(FileTest3, stack_files_with_zfile) {
    CleanUp();
    cout << "generating " << FLAGS_layers << " RO layers by randwrite()" << endl;
    for (int i = 0; i < FLAGS_layers; ++i) {
        files[i] = create_commit_layer(0, 0 , true,
                                       false);
    }
    cout << "verifying stacked RO layers file" << endl;
    auto lower = open_files_ro(files, FLAGS_layers);
    ((LSMTReadOnlyFile *)lower)->m_index =
        create_level_index(lower->index()->buffer(), lower->index()->size(), 0, UINT64_MAX, false);
    EXPECT_EQ(((LSMTReadOnlyFile *)lower)->close_seal(), -1);
    CommitArgs _(nullptr);
    EXPECT_EQ(((LSMTReadOnlyFile *)lower)->commit(_), -1);
    auto stat = ((LSMTReadOnlyFile *)lower)->data_stat();
    LOG_INFO("RO valid data: `", stat.valid_data_size);
    cout << "generating a RW layer by randwrite()" << endl;
    auto upper = create_a_layer();
    auto file = stack_files(upper, lower, 0, true);
    verify_file(file);
    delete file;
}

TEST_F(FileTest3, stack_files_with_zfile_checksum) {
    CleanUp();
    cout << "generating " << FLAGS_layers << " RO layers by randwrite()" << endl;
    for (int i = 0; i < FLAGS_layers; ++i) {
        files[i] = create_commit_layer(0, 0 , true,
                                       true);
    }
    cout << "verifying stacked RO layers file" << endl;
    auto lower = open_files_ro(files, FLAGS_layers);
    ((LSMTReadOnlyFile *)lower)->m_index =
        create_level_index(lower->index()->buffer(), lower->index()->size(), 0, UINT64_MAX, false);
    EXPECT_EQ(((LSMTReadOnlyFile *)lower)->close_seal(), -1);
    CommitArgs _(nullptr);
    EXPECT_EQ(((LSMTReadOnlyFile *)lower)->commit(_), -1);
    auto stat = ((LSMTReadOnlyFile *)lower)->data_stat();
    LOG_INFO("RO valid data: `", stat.valid_data_size);
    cout << "generating a RW layer by randwrite()" << endl;
    auto upper = create_a_layer();
    auto file = stack_files(upper, lower, 0, true);
    verify_file(file);
    delete file;
}

TEST_F(FileTest3, photon_verify) {
    reset_verify_file();
    printf("create image..\n");
    for (int i = 0; i < (int)IMAGE_RO_LAYERS; i++) {
        files[i] = create_commit_layer();
    }
    auto lower = open_files_ro(files, IMAGE_RO_LAYERS);
    auto top_layer = create_file_rw();
    unique_ptr<IFileRW> flsmt(stack_files(top_layer, lower));
    flsmt->set_index_group_commit(4096);
    flsmt->set_max_io_size(511 * 1024); // invalid
    LOG_INFO("max_io_size: `", flsmt->get_max_io_size());
    flsmt->set_max_io_size(512 * 1024);
    LOG_INFO("max_io_size: `", flsmt->get_max_io_size());
    int thread_cnt = FLAGS_threads;
    LOG_INFO("start multi-threads test, jobs: `", thread_cnt);
    vector<photon::thread *> threads;
    errno = 0;

    for (int i = 0; i < thread_cnt; i++) {

        threads.push_back(photon::thread_create11(&FileTest2::randwrite, (FileTest2 *)this,
                                                  flsmt.get(), FLAGS_nwrites / thread_cnt));
        photon::thread_enable_join(threads.back());
    }
    auto stat = flsmt->data_stat();
    LOG_INFO("valid_size: `", stat.valid_data_size);

    if (errno != 0) {
        LOG_INFO("previous err: `(`)", errno, strerror(errno));
    }
    for (auto thd : threads)
        thread_join((photon::join_handle *)thd);
    if (errno != 0) {
        LOG_INFO("previous err: `(`)", errno, strerror(errno));
    }
    errno = 0;
    threads.clear();
    for (int i = 0; i < thread_cnt; i++) {
        LOG_INFO("vsize: `", vsize);
        bool (FileTest2::*fn)(IFileRO *) = &FileTest2::verify_file;
        threads.push_back(photon::thread_create11(fn, (FileTest2 *)this, flsmt.get()));
        photon::thread_enable_join(threads.back());
    }
    for (auto thd : threads)
        thread_join((photon::join_handle *)thd);
}

int main(int argc, char **argv) {

    auto seed = 154574045;
    cerr << "seed = " << seed << endl;
    srand(seed);

    ::testing::InitGoogleTest(&argc, argv);
    ::gflags::ParseCommandLineFlags(&argc, &argv, true);
    log_output_level = FLAGS_log_level;
    photon::init();
    photon::fd_events_epoll_init();
    photon::libaio_wrapper_init();

    auto ret = RUN_ALL_TESTS();
    (void)ret;

    photon::libaio_wrapper_fini();

    photon::fd_events_epoll_fini();
    photon::fini();
    return 0;
}