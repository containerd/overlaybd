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
#include "photon/fs/localfs.h"
#include <fcntl.h>
#include <sys/time.h>
#include <photon/photon.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

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

#define RAND_RANGE (uint64_t)(rand() % ((32 << 20) - 128)), (uint32_t)(rand() % (1 << 6) + 1)

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
        auto ci = create_combo_index(idx0, mi, k, false);
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

void test_combo(const IMemoryIndex *indexes[], size_t ni, const SegmentMapping stdrst[],
                size_t nrst) {
    auto i0 = create_memory_index0(indexes[0]->buffer(), indexes[0]->size(), 0, 1000000);
    auto mi = merge_memory_indexes(indexes + 1, ni - 1);
    ComboIndex ci((Index0 *)i0, (Index *)mi, ni - 1, true);
    SegmentMapping pm[20];
    assert(LEN(pm) >= nrst);
    auto ret = ci.lookup(Segment{0, 10000}, pm, LEN(pm));
    EXPECT_EQ(ret, nrst);
    if (ret == nrst) {
        auto ret = memcmp(pm, stdrst, nrst * sizeof(stdrst[0]));
        EXPECT_EQ(ret, 0);
    }
    LOG_INFO("make RO index of ci");
    auto ro_idx = ci.make_read_only_index();
    if (ro_idx->size() == nrst) {
        auto ret = memcmp(ro_idx->buffer(), stdrst, nrst * sizeof(stdrst[0]));
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
    // test_merge(indexes, ni, stdrst, NR);
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

    Index idx0(mapping0, LEN(mapping0), false);
    Index idx1(mapping1, LEN(mapping1), false);
    Index idx2(mapping2, LEN(mapping2), false);
    Index idx3(mapping3, LEN(mapping3), false);
    const IMemoryIndex *indexes[] = {&idx0, &idx1, &idx2, &idx3};

    test_merge_combo(indexes, 2,
                     {{0, 1, 7, 0},
                      {2, 3, 5, 0},
                      {5, 5, 0, 1},
                      {10, 10, 50, 1},
                      {20, 5, 22 + 5, 0},
                      {30, 15, 89, 0},
                      {87, 13, 32, 0},
                      {100, 10, 20, 1},
                      {110, 27, 55, 0},
                      {150, 10, 84, 0}});
    test_merge_combo(indexes, 3,
                     {{0, 1, 7, 0},
                      {1, 1, 134, 1},
                      {2, 3, 5, 0},
                      {5, 5, 0, 2},
                      {10, 10, 50, 2},
                      {20, 5, 22 + 5, 0},
                      {25, 5, 320 + 7, 1},
                      {30, 15, 89, 0},
                      {45, 42, 320 + 27, 1},
                      {87, 13, 32, 0},
                      {100, 10, 20, 2},
                      {110, 27, 55, 0},
                      {137, 13, 4893 + 37, 1},
                      {150, 10, 84, 0},
                      {160, 40, 4893 + 60, 1},
                      {1000, 1000, 39823, 1}});
    test_merge_combo(indexes, 4,
                     {{0, 1, 7, 0},
                      {1, 1, 134, 1},
                      {2, 3, 5, 0},
                      {5, 5, 0, 3},
                      {10, 10, 50, 3},
                      {20, 5, 22 + 5, 0},
                      {25, 5, 320 + 7, 1},
                      {30, 15, 89, 0},
                      {45, 42, 320 + 27, 1},
                      {87, 13, 32, 0},
                      {100, 10, 20, 3},
                      {110, 27, 55, 0},
                      {137, 13, 4893 + 37, 1},
                      {150, 10, 84, 0},
                      {160, 40, 4893 + 60, 1},
                      {230, 43, 432, 2},
                      {1000, 1000, 39823, 1},
                      {2000, 30, 2393 + 1, 2}});
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

TEST_F(FileTest, create_open_sp) {
    auto file1 = create_file_rw(/*sparse = */ true);
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

TEST_F(FileTest2, sparse_rw) {
    auto fn_sparse = "sparse_test.lsmt";
    // auto fn_normal = "rw.lsmt";
    auto file = lfs->open(fn_sparse, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    if (!file) {
        LOG_ERROR("create file failed: `, err: `", fn_sparse, strerror(errno));
        return;
    }
    LayerInfo args;
    args.fdata = file;
    args.sparse_rw = true;
    args.virtual_size = 64 << 20;
    auto layer = ::create_file_rw(args, true);
    char raw_data[65536];
    vector<Segment> segments{{5, 5},        {10, 10}, {20, 10}, {100, 10},
                             {130944, 128}, {7, 8},   {25, 80}};
    // vector<uint64_t> predict_write{5, 10, 10, 10, 128, 0, 70};
    // {{5, 25, 0}, {100, 10, 20}});
    int i = 0;
    auto rw_index = layer->index();

    for (auto m : segments) {
        // Segment s{(uint64_t)m.offset, (uint32_t)m.length};
        // auto predict = predict_insert(rw_index, s, 65536);
        // auto check = predict_write[i];
        // EXPECT_EQ(predict, check);
        layer->pwrite(raw_data, m.length * ALIGNMENT, m.offset * ALIGNMENT);
        i++;
    }
    SegmentMapping *segs = rw_index->dump();
    for (auto m = segs; m < segs + rw_index->size(); m++) {
        LOG_INFO("`", *m);
    }
    layer->close();
    file = lfs->open(fn_sparse, O_RDONLY);
    layer = ::open_file_rw(file, nullptr, true);
    rw_index = layer->index();
    segs = rw_index->dump();
    for (auto m = segs; m < segs + rw_index->size(); m++) {
        LOG_INFO("`", *m);
    }
    layer->close();
}

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

TEST_F(FileTest2, commit) {
    reset_verify_file();
    auto file0 = create_file_rw();
    auto file1 = create_file_rw(true);
    randwrite1(file0, file1, FLAGS_nwrites);
    LOG_INFO("compare index.");
    auto index0 = file0->index();
    auto index1 = file1->index();
    auto p0 = index0->dump();
    auto p1 = index1->dump();
    EXPECT_EQ(index0->size(), index1->size());
    for (size_t i = 0; i < index0->size(); i++) {
        auto s0 = p0 + i;
        auto s1 = p1 + i;
        EXPECT_EQ(s0->offset, s1->offset);
        EXPECT_EQ(s0->length, s1->length);
    }
    auto fn_c0 = "commit0";
    auto fn_c1 = "commit1";
    verify_file(open_file_rw());
    auto fcommit0 = lfs->open(fn_c0, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    auto fcommit1 = lfs->open(fn_c1, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    CommitArgs args0(fcommit0), args1(fcommit1);
    file0->commit(args0);
    file1->commit(args1);
    LOG_INFO("verify commit file from append LSMT");
    verify_file(fn_c0);
    LOG_INFO("verify commit file from sparse LSMT");
    verify_file(fn_c1);
    DEFER(lfs->unlink(fn_c0));
    DEFER(lfs->unlink(fn_c1));
}

TEST_F(FileTest2, commit_zfile) {
    reset_verify_file();

    auto *file = create_file();
    auto fn_c0 = "commit0";
    auto fn_c1 = "commit1";
    auto fcommit0 = lfs->open(fn_c0, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    auto fcommit1 = lfs->open(fn_c1, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    CompressOptions opt;
    opt.verify = 1;
    CompressArgs zfile_args(opt);
    auto fstream_zfile = ZFile::new_zfile_builder(fcommit1, &zfile_args);
    CommitArgs args1(fstream_zfile);
    // CommitArgs args1(fcommit1);

    LOG_INFO("start zfileBuilder commit");
    file->commit(args1);
    fstream_zfile->close();
    file->close();
    LOG_INFO("verify commit file from StreamingZFile");
    // zfile_compress(fcommit1, fcommit0, &zfile_args);
    auto zfile = ZFile::zfile_open_ro(fcommit1);
    file = (IFileRW *)::open_file_ro(zfile);
    // file = (IFileRW*)open_file_ro(fn_c1);
    verify_file((IFileRO *)file);
    delete zfile;
    delete file;
}

TEST_F(FileTest3, stack_files) {
    CleanUp();
    cout << "generating " << FLAGS_layers << " RO layers by randwrite()" << endl;
    for (int i = 0; i < FLAGS_layers; ++i) {
        files[i] = create_commit_layer(0, ut_io_engine);
    }

    cout << "merging RO layers as " << fn_merged << endl;
    auto merged = lfs->open(fn_merged, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    merge_files_ro(files, FLAGS_layers, merged);
    auto mergedro =::open_file_ro(merged, true);
    cout << "verifying merged RO layers file" << endl;
    verify_file(mergedro);
    delete mergedro;
    cout << "verifying stacked RO layers file" << endl;
    auto lower = open_files_ro(files, FLAGS_layers);
    verify_file(lower);
    ((LSMTReadOnlyFile *)lower)->m_index =
        create_level_index(lower->index()->buffer(), lower->index()->size(), 0, UINT64_MAX, false);
    EXPECT_EQ(((LSMTReadOnlyFile *)lower)->close_seal(), -1);
    CommitArgs _(nullptr);
    auto stat = ((LSMTReadOnlyFile *)lower)->data_stat();
    LOG_INFO("RO valid data: `", stat.valid_data_size);
    merged = lfs->open(fn_merged, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    EXPECT_EQ(lower->flatten(merged), 0);
    // auto localfile = lfs->open(fn_merged, O_RDONLY);
    struct stat st;
    merged->fstat(&st);
    cout << "verifying flattened layer of lowers, st_size: "<<st.st_size << endl;
    verify_file(fn_merged);
    delete merged;
    cout << "generating a RW layer by randwrite()" << endl;
    auto upper = create_file_rw();
    auto file = stack_files(upper, lower, 0, true);
    randwrite(file, FLAGS_nwrites);
    verify_file(file);

    merged = lfs->open(fn_merged, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    file->flatten(merged);
    merged->fstat(&st);
    cout << "verifying flattened layer of stacked layers, st_size: "<<st.st_size << endl;
    merged->close();

    verify_file(fn_merged);
    delete file;
    delete merged;
}

TEST_F(FileTest3, seek_data) {
    CleanUp();
    cout << "generating " << FLAGS_layers << " RO layers by randwrite()" << endl;
    for (int i = 0; i < FLAGS_layers; ++i) {
        files[i] = create_commit_layer(0, ut_io_engine);
    }

    auto lower = open_files_ro(files, FLAGS_layers);
    verify_file(lower);
    ((LSMTReadOnlyFile *)lower)->m_index =
        create_level_index(lower->index()->buffer(), lower->index()->size(), 0, UINT64_MAX, false);
    cout << "generating a RW layer by randwrite()" << endl;
    auto upper = create_file_rw();
    auto file = stack_files(upper, lower, 0, true);
    randwrite(file, FLAGS_nwrites);
    verify_file(file);

    auto fmerged = create_file_rw();
    vector<Segment> segs;
    auto data = new char[8<<20];
    file->seek_data(0, vsize, segs);
    LOG_INFO("flattern segments count: `", segs.size());
    for (auto m : segs) {
        auto readn = file->pread(data, m.length*ALIGNMENT, m.offset * ALIGNMENT);
        EXPECT_EQ(readn, fmerged->pwrite(data, readn, m.offset * ALIGNMENT));
    }
    verify_file(fmerged);
    delete file;
    delete fmerged;
    delete[] data;
}


TEST_F(FileTest3, sparsefile_close_seal) {
    CleanUp();
    cout << "generating " << FLAGS_layers << " RO layers by randwrite()" << endl;
    auto file = create_a_layer(true);
    IFileRO *fdup;
    file->close_seal(&fdup);
    verify_file(fdup);
    auto fcommit = lfs->open(layer_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    CommitArgs args(fcommit);
    ((LSMTReadOnlyFile*)fdup)->commit(args);
    fcommit->close();
    fcommit = (IFile*)open_file_ro(layer_name.back().c_str());
    verify_file((IFileRO*)fcommit);
    delete fcommit;
    delete file;
}

TEST_F(FileTest3, stack_sparsefiles) {
    CleanUp();
    cout << "generating " << FLAGS_layers << " RO layers by randwrite()" << endl;
    for (int i = 0; i < FLAGS_layers; ++i) {
        files[i] = create_commit_layer(0, 1 /*libaio*/, false, false,
                                       true); //创建若干层RO Layer,文件指针保存在files中
        auto lower = open_files_ro(files, i + 1);
        DEFER(delete lower);
        verify_file(lower);
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
    auto upper = create_a_layer(true);

    auto file = stack_files(upper, lower, 0, true);

    verify_file(file);
    delete file;
}

TEST_F(FileTest3, stack_files_with_zfile) {
    CleanUp();
    cout << "generating " << FLAGS_layers << " RO layers by randwrite()" << endl;
    for (int i = 0; i < FLAGS_layers; ++i) {
        files[i] = create_commit_layer(0, 0, true, false);
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
    auto upper = create_file_rw();
    auto file = stack_files(upper, lower, 0, true);
    randwrite(file, FLAGS_nwrites);
    verify_file(file);
    delete file;
}

TEST_F(FileTest3, stack_files_with_zfile_checksum) {
    CleanUp();
    cout << "generating " << FLAGS_layers << " RO layers by randwrite()" << endl;
    for (int i = 0; i < FLAGS_layers; ++i) {
        files[i] = create_commit_layer(0, ut_io_engine, true, true);
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
    auto upper = create_file_rw();
    auto file = stack_files(upper, lower, 0, true);
    randwrite(file, FLAGS_nwrites);
    verify_file(file);
    delete file;
}

TEST_F(FileTest3, photon_verify) {
    reset_verify_file();
    printf("create image..\n");
    for (int i = 0; i < (int)FLAGS_layers; i++) {
        files[i] = create_commit_layer();
    }
    auto lower = open_files_ro(files, FLAGS_layers);
    auto top_layer = create_file_rw();
    unique_ptr<IFileRW> flsmt(stack_files(top_layer, lower));
    flsmt->set_index_group_commit(4096);
    flsmt->set_max_io_size(511 * 1024); // invalid
    LOG_INFO("max_io_size: `", flsmt->get_max_io_size());
    flsmt->set_max_io_size(512 * 1024);
    LOG_INFO("max_io_size: `", flsmt->get_max_io_size());
    int thread_cnt = FLAGS_threads;
    LOG_INFO("start multi-threads test, jobs: `", thread_cnt);
    randwrite(flsmt.get(), FLAGS_nwrites);
    vector<photon::thread *> threads;
    errno = 0;

    if (errno != 0) {
        LOG_INFO("previous err: `(`)", errno, strerror(errno));
    }
    auto stat = flsmt->data_stat();
    LOG_INFO("valid_size: `", stat.valid_data_size);
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

void WarpFileTest::randwrite_warpfile(IFile *file, size_t nwrites) {
    LOG_INFO("start randwrite ` times", nwrites);
    ALIGNED_MEM4K(buf, 1 << 20)
    auto vsize = FLAGS_vsize << 20;
    for (size_t i = 0; i < nwrites; ++i) {
        off_t offset = 0;
        off_t length = gen_write_data(buf, vsize, 128 * 1024, offset);
        auto roll = rand() % 4;
        if (roll == 0) {
            LOG_DEBUG("offset: `, length: `", offset, length);

            ssize_t ret = file->pwrite(buf, length, offset);
            if (ret != length) {
                LOG_ERROR("`(`)", errno, strerror(errno));
                exit(-1);
            }
            EXPECT_EQ(ret, length);
        } else {
            RemoteMapping lba;
            lba.count = length;
            lba.offset = offset;
            lba.roffset = offset;
            file->ioctl(IFileRW::RemoteData, lba);
        }
        if (FLAGS_verify) {
            // memcpy(data + offset, buf, length);
            fcheck->pwrite(buf, length, offset);
        }
    }
}

IFileRW *WarpFileTest::create_warpfile_rw(int io_engine) {
    name_next_layer();
    auto fmeta = lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    auto findex = lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    LOG_INFO("create warpfile { fn_lba: `, fn_meta `}", data_name.back().c_str(),
             idx_name.back().c_str());
    WarpFileArgs args(findex, fmeta, fcheck);
    args.virtual_size = FLAGS_vsize << 20;
    memcpy(args.parent_uuid.data, parent_uuid.c_str(), parent_uuid.length());
    auto file = create_warpfile(args, false);
    return file;
}

IFileRO *WarpFileTest::create_commit_warpfile(IFileRW* warpfile, bool keepUUID) {
    LOG_INFO("commit warpfile as `", layer_name.back().c_str());
    auto fcommit = lfs->open(layer_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    UUID uu;
    uu.parse(parent_uuid.c_str(), parent_uuid.size());
    CommitArgs c(fcommit);
    c.parent_uuid = uu;

    // commit will clear layer's uuid if CommitArgs.uuid is not specify
    if (keepUUID) {
        warpfile->get_uuid(uu);
        c.uuid = uu;
    }
    ((IFileRW *)warpfile)->commit(c);
    delete warpfile;
    fcommit->close();
    fcommit = lfs->open(layer_name.back().c_str(), O_RDONLY);
    auto ret = open_warpfile_ro(fcommit, fcheck, true);
    ret->get_uuid(uu);
    parent_uuid = UUID::String(uu).c_str();
    return ret;
}

IFileRO *WarpFileTest::create_commit_warpfile(int io_engine, bool keepUUID) {
    auto warpfile = create_warpfile_rw(io_engine);
    randwrite_warpfile(warpfile, FLAGS_nwrites);
    return create_commit_warpfile(warpfile, keepUUID);
}

TEST_F(WarpFileTest, randwrite) {
    CleanUp();
    log_output_level = FLAGS_log_level;
    LOG_INFO("log level: `", log_output_level);
    auto file = create_warpfile_rw(ut_io_engine);
    randwrite_warpfile(file, FLAGS_nwrites);
    verify_file(file);
    file->close();
    // auto findex = lfs->open("rwtmp.index", O_RDWR | O_CREAT, S_IRWXU);
    auto fmeta = lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT, S_IRWXU);
    auto findex = lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT, S_IRWXU);
    file = open_warpfile_rw(findex, fmeta, fcheck, false);
    verify_file(file);
    file->close();
    LOG_INFO("commit warpfile as `", layer_name.back().c_str());
    auto fcommit = lfs->open(layer_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    file = open_warpfile_rw(findex, fmeta, nullptr, true);
    CommitArgs c(fcommit);
    ((IFileRW *)file)->commit(c);
    file->close();
    verify_file(open_warpfile_ro(fcommit, fcheck, true));
    fcheck = nullptr;
    delete file;
}

TEST_F(WarpFileTest, commit) {
    CleanUp();
    auto file = create_commit_warpfile(ut_io_engine);
    verify_file(file);
    fcheck = nullptr; // delete by warpfile.
    delete file;
}

TEST_F(WarpFileTest, large_lba) {
    CleanUp();
    log_output_level = FLAGS_log_level;
    LOG_INFO("log level: `", log_output_level);
    auto fidx = open_localfile_adaptor("/tmp/warpfile.idx", O_TRUNC | O_CREAT | O_RDWR);
    // auto flba = open_localfile_adaptor("/tmp/warpfile.lba", O_TRUNC | O_CREAT | O_RDWR);
    auto fmeta = open_localfile_adaptor("/tmp/warpfile.meta", O_TRUNC | O_CREAT | O_RDWR);
    WarpFileArgs args(fidx, fmeta, fcheck);
    args.virtual_size = FLAGS_vsize << 20;
    // Set memory file
    auto file = create_warpfile(args, true);
    DEFER(delete file);
    RemoteMapping lba;
    lba.count = 26 << 20; // 26MB
    lba.offset = 0;
    lba.roffset = 0;
    file->ioctl(IFileRW::RemoteData, lba);
    EXPECT_EQ(((LSMTWarpFile *)file)->m_index->size(), 4);
    uint32_t LEN = Segment::MAX_LENGTH;
    const static SegmentMapping check[] = {{0, LEN, 0, 1},
                                           {LEN, LEN, LEN, 1},
                                           {LEN * 2, LEN, LEN * 2, 1},
                                           {LEN * 3, lba.count / ALIGNMENT - LEN * 3, LEN * 3, 1}};
    auto p = ((IMemoryIndex0 *)(((LSMTWarpFile *)file)->m_index))->dump();
    for (int i = 0; i < 4; i++) {
        LOG_INFO("check: `, mapping: `", check[i], p[i]);
    }
    EXPECT_EQ(memcmp((void *)check, (void *)p, 4 * sizeof(SegmentMapping)), 0);
    fcheck = nullptr;
}

TEST_F(WarpFileTest, multi_layer) {
    CleanUp();
    log_output_level = FLAGS_log_level;
    LOG_INFO("log level: `", log_output_level);
    for (int i = 0; i < FLAGS_layers; ++i) {
        files[i] = create_commit_warpfile(ut_io_engine);
    }
    auto file = open_files_ro(files, FLAGS_layers);
    verify_file(file);
    delete file;
    fcheck = nullptr;
}

TEST_F(WarpFileTest, stack_files) {
    CleanUp();
    log_output_level = FLAGS_log_level;
    LOG_INFO("log level: `", log_output_level);
    int k = 0;
    for (int i = 0; i < FLAGS_layers; ++i) {
        auto roll = rand() % 2;
        if (roll || i == FLAGS_layers - 1) {
            k++;
            files[i] = create_commit_warpfile(ut_io_engine);
        } else {
            files[i] = create_commit_layer();
        }
    }
    LOG_INFO("creat ` layers with ` warp layers.", FLAGS_layers, k);
    auto lower = open_files_ro(files, FLAGS_layers);
    verify_file(lower);
    LOG_INFO("create top RW layer by randwrite()");
    auto upper = create_warpfile_rw();
    auto file = stack_files(upper, lower, false, true);
    randwrite_warpfile(file, FLAGS_nwrites);
    verify_file(file);
    LOG_INFO("commit top layer.");
    delete upper;
    auto fmeta = lfs->open(data_name.back().c_str(), O_RDONLY, S_IRWXU);
    auto findex = lfs->open(idx_name.back().c_str(), O_RDONLY, S_IRWXU);
    upper = open_warpfile_rw(findex, fmeta, nullptr, false);
    files[FLAGS_layers] = create_commit_warpfile(upper);
    delete lower;
    lower = open_files_ro(files, FLAGS_layers + 1);
    verify_file(lower);
    delete file;
    fcheck = nullptr;
}

TEST_F(WarpFileTest, commit_without_uuid) {
    CleanUp();
    UUID uu;
    auto frw = create_warpfile_rw(ut_io_engine);
    frw->get_uuid(uu);
    LOG_INFO("uuid before commit is `", uu);
    EXPECT_FALSE(uu.is_null());

    auto fcommit = create_commit_warpfile(frw);
    fcommit->get_uuid(uu);
    LOG_INFO("uuid after commit is `", uu);
    EXPECT_TRUE(uu.is_null());
}

TEST_F(WarpFileTest, commit_with_uuid) {
    CleanUp();
    UUID uu;
    auto frw = create_warpfile_rw(ut_io_engine);
    frw->get_uuid(uu);
    LOG_INFO("uuid before commit is `", uu);
    EXPECT_FALSE(uu.is_null());

    UUID uu_commit;
    auto fcommit = create_commit_warpfile(frw, true);
    fcommit->get_uuid(uu_commit);
    LOG_INFO("uuid after commit is `", uu);
    EXPECT_FALSE(uu_commit.is_null());
    EXPECT_EQ(uu, uu_commit);
}

int main(int argc, char **argv) {

    auto seed = 154574045;
    cerr << "seed = " << seed << endl;
    srand(seed);

    ::testing::InitGoogleTest(&argc, argv);
    ::gflags::ParseCommandLineFlags(&argc, &argv, true);
    log_output_level = FLAGS_log_level;
    LOG_INFO("log level: `", log_output_level);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);

    auto ret = RUN_ALL_TESTS();
    (void)ret;

    return 0;
}
