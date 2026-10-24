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
#include <photon/fs/forwardfs.h>
#include <fcntl.h>
#include <thread>
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

TEST_F(FileTest, reject_oversized_index) {
    const char *filename = "oversized-index.lsmt";
    auto file = lfs->open(filename, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    ASSERT_NE(file, nullptr);

    LayerInfo info;
    info.virtual_size = HeaderTrailer::SPACE;
    ASSERT_EQ(write_header_trailer(file, true, true, true, 0, 0, info),
              (int)HeaderTrailer::SPACE);

    const uint64_t index_size = MAX_LSMT_RO_INDEX_SIZE + 1;
    const uint64_t index_offset = HeaderTrailer::SPACE;
    const uint64_t trailer_offset =
        index_offset + index_size * sizeof(SegmentMapping);
    ASSERT_EQ(file->lseek(trailer_offset, SEEK_SET), (off_t)trailer_offset);
    ASSERT_EQ(write_header_trailer(file, false, true, true, index_offset,
                                   index_size, info),
              (int)HeaderTrailer::SPACE);

    EXPECT_EQ(LSMT::open_file_ro(file), nullptr);
    delete file;
    lfs->unlink(filename);
}

TEST_F(FileTest, reject_oversized_rw_index) {
    auto rw = create_file_rw();
    ASSERT_NE(rw, nullptr);
    delete rw;

    auto fdata = lfs->open(data_name.back().c_str(), O_RDWR, S_IRWXU);
    auto findex = lfs->open(idx_name.back().c_str(), O_RDWR, S_IRWXU);
    ASSERT_NE(fdata, nullptr);
    ASSERT_NE(findex, nullptr);

    const uint64_t index_file_size =
        HeaderTrailer::SPACE +
        (MAX_LSMT_INDEX_SIZE + 1) * sizeof(SegmentMapping);
    ASSERT_EQ(findex->ftruncate(index_file_size), 0);

    auto reopened = LSMT::open_file_rw(fdata, findex, false);
    EXPECT_EQ(reopened, nullptr);

    delete reopened;
    delete fdata;
    delete findex;
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

TEST(Index, reject_oversized_merge) {
    SegmentMapping mapping0[] = {{0, 1, 0}, {2, 1, 2}};
    SegmentMapping mapping1[] = {{1, 1, 1}, {3, 1, 3}};

    Index index0(mapping0, LEN(mapping0), false);
    Index index1(mapping1, LEN(mapping1), false);
    const IMemoryIndex *indexes[] = {&index0, &index1};

    const auto old_max_index_size = LSMT::test_max_lsmt_index_size;
    LSMT::test_max_lsmt_index_size = 3;

    IMemoryIndex *merged = nullptr;
    EXPECT_NO_THROW(merged = merge_memory_indexes(indexes, LEN(indexes)));

    LSMT::test_max_lsmt_index_size = old_max_index_size;

    EXPECT_EQ(merged, nullptr);
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
    auto file1 = create_file_rw(RWType::Sparse);
    delete file1;
    auto file2 = open_file_rw();
    delete file2;
}

TEST_F(FileTest, create_open_hybrid) {
    auto file1 = create_file_rw(RWType::Hybrid);
    delete file1;
    auto file2 = open_file_rw();
    delete file2;
}

TEST_F(FileTest, create_unknown_rw_type) {
    name_next_layer();
    auto fdata = lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    auto findex = lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    DEFER(delete fdata);
    DEFER(delete findex);
    LayerInfo args(fdata, findex);
    args.virtual_size = vsize;
    args.rw_type = (RWType)0xff;
    LOG_INFO("TEST: now create a rw layer with unknown rw type.. expected ret: nullptr");
    EXPECT_EQ(nullptr, ::create_file_rw(args, true));
}

// a single I/O of a hybrid RW test case, offset and length are in ALIGNMENT-sized blocks
struct HybridIO {
    uint64_t offset;
    uint32_t length;
    // the byte filling the range, 0 means discard (fallocate) instead of write
    unsigned char pattern;
};

struct HybridCase {
    const char *name;
    vector<HybridIO> seed;    // prepares the layout of the writable layer
    HybridIO overwrite;       // the I/O of which the layer growth is measured
    size_t appended_blocks;   // expected growth of the data file, in blocks
    size_t appended_mappings; // expected growth of the index file, in mappings
};

class HybridFileTest : public FileTest {
public:
    // large enough for every case below
    static const size_t MAX_IMAGE_SIZE = 512 * ALIGNMENT;

    // the expected content of the image, updated by every do_io()
    vector<char> expected;

    void do_io(IFileRW *file, const HybridIO &io) {
        auto offset = (size_t)io.offset * ALIGNMENT;
        auto size = (size_t)io.length * ALIGNMENT;
        ASSERT_LE(offset + size, MAX_IMAGE_SIZE);
        if (expected.size() < offset + size)
            expected.resize(offset + size, 0);
        if (io.pattern == 0) {
            ASSERT_EQ(0, file->fallocate(3, offset, size));
        } else {
            ALIGNED_MEM4K(data, MAX_IMAGE_SIZE);
            memset(data, io.pattern, size);
            ASSERT_EQ((ssize_t)size, file->pwrite(data, size, offset));
        }
        memset(&expected[offset], io.pattern, size);
    }

    void verify(IFileRO *file) {
        ALIGNED_MEM4K(readback, MAX_IMAGE_SIZE);
        ASSERT_EQ((ssize_t)expected.size(), file->pread(readback, expected.size(), 0));
        EXPECT_EQ(0, memcmp(&expected[0], readback, expected.size()));
    }
};

const size_t HybridFileTest::MAX_IMAGE_SIZE;

// A hybrid RW layer rewrites the data of the mappings it already owns in place, and
// appends everything else: holes, zeroed ranges and the ranges of the lower layers.
TEST_F(HybridFileTest, hybrid_rw_overwrite) {
    vector<HybridCase> cases{
        {"rewrite a part of a mapping", {{0, 64, 0x11}}, {8, 48, 0x22}, 0, 0},
        {"rewrite a whole mapping", {{32, 64, 0x11}}, {32, 64, 0x22}, 0, 0},
        {"rewrite a mapping and append the tail", {{0, 64, 0x11}}, {0, 80, 0x22}, 16, 1},
        {"rewrite a mapping and append both sides", {{32, 32, 0x11}}, {0, 96, 0x22}, 64, 2},
        {"append the hole between two mappings",
         {{0, 64, 0x11}, {128, 128, 0x22}},
         {32, 160, 0x33},
         64,
         1},
        {"append the exact hole between two mappings",
         {{0, 64, 0x11}, {128, 64, 0x22}},
         {64, 64, 0x33},
         64,
         1},
        {"append the two holes among three mappings",
         {{0, 64, 0x11}, {128, 64, 0x22}, {256, 64, 0x44}},
         {32, 256, 0x33},
         128,
         2},
        {"append a zeroed mapping and a hole",
         {{0, 64, 0x11}, {64, 64, /* discard = */ 0}},
         {32, 128, 0x22},
         96,
         2},
    };

    for (auto &c : cases) {
        LOG_INFO("hybrid RW case: `", c.name);
        expected.clear();
        auto file = create_file_rw(RWType::Hybrid);
        ASSERT_NE(nullptr, file);
        for (auto &io : c.seed)
            do_io(file, io);
        auto data_size = file_size(lfs, data_name.back().c_str());
        auto index_size = file_size(lfs, idx_name.back().c_str());

        do_io(file, c.overwrite);
        EXPECT_EQ(data_size + (ssize_t)(c.appended_blocks * ALIGNMENT),
                  file_size(lfs, data_name.back().c_str()));
        EXPECT_EQ(index_size + (ssize_t)(c.appended_mappings * sizeof(SegmentMapping)),
                  file_size(lfs, idx_name.back().c_str()));
        verify(file);
        delete file;

        // the rewritten data and the index must survive a reopen, and the reopened
        // layer must keep rewriting in place
        auto reopened = open_file_rw();
        ASSERT_NE(nullptr, reopened);
        verify(reopened);
        data_size = file_size(lfs, data_name.back().c_str());
        do_io(reopened, c.overwrite);
        EXPECT_EQ(data_size, file_size(lfs, data_name.back().c_str()));
        verify(reopened);
        delete reopened;
    }
}

// A hybrid upper layer still rewrites its own mappings in place after stack_files(),
// while the ranges of the sealed lower layers are always appended.
TEST_F(HybridFileTest, hybrid_rw_stacked_layers) {
    auto lower_rw = create_file_rw();
    ASSERT_NE(nullptr, lower_rw);
    do_io(lower_rw, {64, 128, 0x11});
    IFileRO *lower = nullptr;
    ASSERT_EQ(0, lower_rw->close_seal(&lower));
    delete lower_rw;
    auto lower_data_size = file_size(lfs, data_name.front().c_str());

    auto upper = create_file_rw(RWType::Hybrid);
    ASSERT_NE(nullptr, upper);
    auto file = stack_files(upper, lower, /* ownership = */ true, /* check_order = */ false);
    ASSERT_NE(nullptr, file);
    DEFER(delete file);
    do_io(file, {0, 64, 0x22});
    auto data_size = file_size(lfs, data_name.back().c_str());

    // [0, 64) belongs to the upper layer and is rewritten in place, while [64, 128)
    // is only mapped by the lower layer and has to be appended
    do_io(file, {0, 128, 0x33});
    EXPECT_EQ(data_size + (ssize_t)(64 * ALIGNMENT), file_size(lfs, data_name.back().c_str()));
    EXPECT_EQ(lower_data_size, file_size(lfs, data_name.front().c_str()));
    verify(file);
}

class FailOncePwriteFile : public photon::fs::ForwardFile_Ownership {
public:
    bool fail_next = false;
    explicit FailOncePwriteFile(IFile *file) : ForwardFile_Ownership(file, true) {
    }
    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        if (fail_next) {
            fail_next = false;
            errno = EIO;
            return -1;
        }
        return m_file->pwrite(buf, count, offset);
    }
};

class ShortThenFailPwriteFile : public photon::fs::ForwardFile_Ownership {
public:
    int failures_left = 0;
    explicit ShortThenFailPwriteFile(IFile *file) : ForwardFile_Ownership(file, true) {
    }
    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        if (failures_left == 2) {
            failures_left--;
            return m_file->pwrite(buf, count / 2, offset);
        }
        if (failures_left == 1) {
            failures_left--;
            errno = EIO;
            return -1;
        }
        return m_file->pwrite(buf, count, offset);
    }
};

TEST_F(HybridFileTest, index_log_error_retry) {
    name_next_layer();
    auto fdata = lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    auto findex = new ShortThenFailPwriteFile(
        lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU));
    LayerInfo args(fdata, findex);
    args.rw_type = RWType::Hybrid;
    args.virtual_size = MAX_IMAGE_SIZE;
    auto file = ::create_file_rw(args, true);
    ASSERT_NE(nullptr, file);

    ALIGNED_MEM4K(wbuf, ALIGNMENT4K);
    memset(wbuf, 0x5a, ALIGNMENT4K);
    findex->failures_left = 2;
    EXPECT_EQ(-1, file->pwrite(wbuf, ALIGNMENT4K, 0));
    EXPECT_EQ((ssize_t)HeaderTrailer::SPACE, file_size(lfs, idx_name.back().c_str()));
    EXPECT_EQ((ssize_t)ALIGNMENT4K, file->pwrite(wbuf, ALIGNMENT4K, 0));
    delete file;

    auto reopened = open_file_rw();
    ASSERT_NE(nullptr, reopened);
    ALIGNED_MEM4K(rbuf, ALIGNMENT4K);
    EXPECT_EQ((ssize_t)ALIGNMENT4K, reopened->pread(rbuf, ALIGNMENT4K, 0));
    EXPECT_EQ(0, memcmp(wbuf, rbuf, ALIGNMENT4K));
    delete reopened;
}

class FaultyIndexFile : public photon::fs::ForwardFile_Ownership {
public:
    int short_writes = 0, failed_writes = 0;
    bool fail_truncate = false;
    explicit FaultyIndexFile(IFile *file) : ForwardFile_Ownership(file, true) {
    }
    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        if (short_writes > 0) {
            short_writes--;
            return m_file->pwrite(buf, count / 2, offset);
        }
        if (failed_writes > 0) {
            failed_writes--;
            errno = EIO;
            return -1;
        }
        return m_file->pwrite(buf, count, offset);
    }
    virtual int ftruncate(off_t length) override {
        if (fail_truncate) {
            errno = EIO;
            return -1;
        }
        return m_file->ftruncate(length);
    }
};

class IndexLogTest : public FileTest {
public:
    FaultyIndexFile *file = nullptr;
    unique_ptr<IndexLog> log;
    int published = 0;

    virtual void SetUp() override {
        FileTest::SetUp();
        name_next_layer();
        file = new FaultyIndexFile(
            lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU));
        log.reset(IndexLog::open(file, true));
        ASSERT_NE(nullptr, log);
    }
    virtual void TearDown() override {
        log.reset();
        FileTest::TearDown();
    }

    static SegmentMapping mapping(uint64_t offset) {
        return SegmentMapping{offset, 8, offset};
    }
    int append(uint64_t offset) {
        return log->append(mapping(offset), [&] { published++; });
    }
    ssize_t log_size() {
        return file_size(lfs, idx_name.back().c_str());
    }
    void expect_log(const vector<SegmentMapping> &expected) {
        ASSERT_EQ((ssize_t)(expected.size() * sizeof(SegmentMapping)), log_size());
        vector<SegmentMapping> actual(expected.size());
        auto size = expected.size() * sizeof(SegmentMapping);
        ASSERT_EQ((ssize_t)size, log->file()->pread(actual.data(), size, 0));
        EXPECT_EQ(0, memcmp(expected.data(), actual.data(), size));
    }
};

TEST_F(IndexLogTest, short_write_completes) {
    file->short_writes = 1;
    ASSERT_EQ(0, append(0));
    EXPECT_EQ(1, published);
    expect_log({mapping(0)});
}

TEST_F(IndexLogTest, failed_append_truncates_and_retries) {
    ASSERT_EQ(0, append(0));
    file->short_writes = 1;
    file->failed_writes = 1;
    EXPECT_EQ(-1, append(8));
    EXPECT_EQ(1, published);
    expect_log({mapping(0)});
    ASSERT_EQ(0, append(8));
    EXPECT_EQ(2, published);
    expect_log({mapping(0), mapping(8)});
}

TEST_F(IndexLogTest, truncate_failure_latches_error) {
    ASSERT_EQ(0, append(0));
    file->short_writes = 1;
    file->failed_writes = 1;
    file->fail_truncate = true;
    EXPECT_EQ(-1, append(8));
    file->fail_truncate = false;
    EXPECT_EQ(-1, append(8));
    EXPECT_EQ(-1, log->flush());
    EXPECT_EQ(1, published);
}

TEST_F(IndexLogTest, group_commit_full_buffer_failure_retries) {
    ASSERT_EQ(0, log->set_group_commit(2 * sizeof(SegmentMapping)));
    ASSERT_EQ(0, append(0));
    EXPECT_EQ(1, published);
    EXPECT_EQ(0, log_size());
    file->failed_writes = 1;
    EXPECT_EQ(-1, append(8));
    EXPECT_EQ(1, published);
    EXPECT_EQ(0, log_size());
    ASSERT_EQ(0, append(8));
    EXPECT_EQ(2, published);
    expect_log({mapping(0), mapping(8)});
}

TEST_F(IndexLogTest, group_commit_flush_pads_and_reports_error) {
    ASSERT_EQ(0, log->set_group_commit(4 * sizeof(SegmentMapping)));
    ASSERT_EQ(0, append(0));
    file->failed_writes = 1;
    EXPECT_EQ(-1, log->flush());
    EXPECT_EQ(0, log_size());
    ASSERT_EQ(0, log->flush());
    auto invalid = SegmentMapping::invalid_mapping();
    expect_log({mapping(0), invalid, invalid, invalid});
    ASSERT_EQ(0, log->flush());
    expect_log({mapping(0), invalid, invalid, invalid});
}

TEST_F(IndexLogTest, shrinking_group_commit_flushes_pending) {
    ASSERT_EQ(0, log->set_group_commit(4 * sizeof(SegmentMapping)));
    ASSERT_EQ(0, append(0));
    ASSERT_EQ(0, append(8));
    EXPECT_EQ(0, log_size());
    ASSERT_EQ(0, log->set_group_commit(sizeof(SegmentMapping)));
    auto invalid = SegmentMapping::invalid_mapping();
    expect_log({mapping(0), mapping(8), invalid, invalid});
}

class CountingDeleteFile : public photon::fs::ForwardFile_Ownership {
public:
    int *deleted;
    CountingDeleteFile(IFile *file, int *deleted)
        : ForwardFile_Ownership(file, true), deleted(deleted) {
    }
    ~CountingDeleteFile() {
        (*deleted)++;
    }
};

// stack_files() hands the index log, together with its group-commit buffer and the
// ownership of the index file, from the upper layer to the stacked file
TEST_F(HybridFileTest, stacking_moves_index_log) {
    auto lower_rw = create_file_rw();
    ASSERT_NE(nullptr, lower_rw);
    do_io(lower_rw, {64, 8, 0x11});
    IFileRO *lower = nullptr;
    ASSERT_EQ(0, lower_rw->close_seal(&lower));
    delete lower_rw;

    name_next_layer();
    int deleted = 0;
    auto fdata = lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    auto findex = new CountingDeleteFile(
        lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU), &deleted);
    LayerInfo args(fdata, findex);
    args.rw_type = RWType::Hybrid;
    args.virtual_size = MAX_IMAGE_SIZE;
    auto upper = ::create_file_rw(args, true);
    ASSERT_NE(nullptr, upper);
    ASSERT_EQ(0, upper->set_index_group_commit(2 * sizeof(SegmentMapping)));
    auto file = stack_files(upper, lower, false, false);
    ASSERT_NE(nullptr, file);

    auto index_size = file_size(lfs, idx_name.back().c_str());
    do_io(file, {0, 8, 0x22});
    EXPECT_EQ(index_size, file_size(lfs, idx_name.back().c_str()));
    do_io(file, {16, 8, 0x33});
    EXPECT_EQ(index_size + (ssize_t)(2 * sizeof(SegmentMapping)),
              file_size(lfs, idx_name.back().c_str()));
    verify(file);

    delete file;
    EXPECT_EQ(1, deleted);
    delete upper;
    EXPECT_EQ(1, deleted);
    delete lower;

    auto reopened = open_file_rw();
    ASSERT_NE(nullptr, reopened);
    ALIGNED_MEM4K(rbuf, 8 * ALIGNMENT);
    ASSERT_EQ((ssize_t)(8 * ALIGNMENT), reopened->pread(rbuf, 8 * ALIGNMENT, 16 * ALIGNMENT));
    EXPECT_EQ(0, memcmp(&expected[16 * ALIGNMENT], rbuf, 8 * ALIGNMENT));
    delete reopened;
}

TEST_F(HybridFileTest, close_seal_flush_error_retry) {
    name_next_layer();
    auto fdata = lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    auto findex = new FailOncePwriteFile(
        lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU));
    LayerInfo args(fdata, findex);
    args.rw_type = RWType::Hybrid;
    args.virtual_size = MAX_IMAGE_SIZE;
    auto file = ::create_file_rw(args, true);
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(0, file->set_index_group_commit(2 * sizeof(SegmentMapping)));

    ALIGNED_MEM4K(wbuf, ALIGNMENT4K);
    memset(wbuf, 0x47, ALIGNMENT4K);
    EXPECT_EQ((ssize_t)ALIGNMENT4K, file->pwrite(wbuf, ALIGNMENT4K, 0));
    findex->fail_next = true;
    IFileRO *sealed = nullptr;
    EXPECT_EQ(-1, file->close_seal(&sealed));
    EXPECT_EQ(nullptr, sealed);
    EXPECT_EQ(0, file->close_seal(&sealed));
    ASSERT_NE(nullptr, sealed);

    delete file;
    delete sealed;
}

TEST_F(HybridFileTest, direct_io_alignment_policy) {
    ALIGNED_MEM4K(wbuf, ALIGNMENT4K);
    memset(wbuf, 0x6b, ALIGNMENT4K);
    for (bool direct : {false, true}) {
        auto file = create_file_rw(RWType::Hybrid);
        ASSERT_NE(nullptr, file);
        ASSERT_EQ(0, file->set_data_file_direct_io(direct));
        EXPECT_EQ((ssize_t)ALIGNMENT, file->pwrite(wbuf, ALIGNMENT, 0));
        auto first_size = file_size(lfs, data_name.back().c_str());
        EXPECT_EQ((ssize_t)(HeaderTrailer::SPACE + (direct ? ALIGNMENT4K : ALIGNMENT)),
                  first_size);
        EXPECT_EQ((ssize_t)ALIGNMENT, file->pwrite(wbuf, ALIGNMENT, 0));
        EXPECT_EQ(first_size + (direct ? ALIGNMENT4K : 0),
                  file_size(lfs, data_name.back().c_str()));
        delete file;
    }
}

TEST_F(HybridFileTest, legacy_unaligned_rewrite_preserves_neighbor) {
    ALIGNED_MEM4K(wbuf, ALIGNMENT4K);
    ALIGNED_MEM4K(rbuf, ALIGNMENT4K);
    for (bool direct : {false, true}) {
        auto file = create_file_rw(RWType::Hybrid);
        ASSERT_NE(nullptr, file);
        ASSERT_EQ(0, file->set_data_file_direct_io(false));
        memset(wbuf, 0x12, ALIGNMENT4K);
        ASSERT_EQ((ssize_t)ALIGNMENT, file->pwrite(wbuf, ALIGNMENT, 0));
        memset(wbuf, 0x23, ALIGNMENT4K);
        ASSERT_EQ((ssize_t)ALIGNMENT, file->pwrite(wbuf, ALIGNMENT, ALIGNMENT4K));
        auto size = file_size(lfs, data_name.back().c_str());
        ASSERT_EQ((ssize_t)(HeaderTrailer::SPACE + 2 * ALIGNMENT), size);

        ASSERT_EQ(0, file->set_data_file_direct_io(direct));
        memset(wbuf, 0x34, ALIGNMENT4K);
        ASSERT_EQ((ssize_t)ALIGNMENT, file->pwrite(wbuf, ALIGNMENT, ALIGNMENT4K));
        EXPECT_EQ(direct ? (ssize_t)(HeaderTrailer::SPACE + 2 * ALIGNMENT4K) : size,
                  file_size(lfs, data_name.back().c_str()));
        delete file;

        auto reopened = open_file_rw();
        ASSERT_NE(nullptr, reopened);
        ASSERT_EQ((ssize_t)ALIGNMENT, reopened->pread(rbuf, ALIGNMENT, 0));
        memset(wbuf, 0x12, ALIGNMENT);
        EXPECT_EQ(0, memcmp(wbuf, rbuf, ALIGNMENT));
        ASSERT_EQ((ssize_t)ALIGNMENT, reopened->pread(rbuf, ALIGNMENT, ALIGNMENT4K));
        memset(wbuf, 0x34, ALIGNMENT);
        EXPECT_EQ(0, memcmp(wbuf, rbuf, ALIGNMENT));
        delete reopened;
    }
}

TEST_F(HybridFileTest, discard_uses_data_eof) {
    name_next_layer();
    auto fdata = new FailOncePwriteFile(
        lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU));
    auto findex = lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    LayerInfo args(fdata, findex);
    args.rw_type = RWType::Hybrid;
    args.virtual_size = MAX_IMAGE_SIZE;
    auto file = ::create_file_rw(args, true);
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(0, file->set_data_file_direct_io(true));

    ALIGNED_MEM4K(wbuf, ALIGNMENT4K);
    memset(wbuf, 0x7c, ALIGNMENT4K);
    fdata->fail_next = true;
    EXPECT_EQ(-1, file->pwrite(wbuf, ALIGNMENT, 0));
    EXPECT_EQ(0, file->fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, ALIGNMENT));
    delete file;

    auto reopened = open_file_rw();
    ASSERT_NE(nullptr, reopened);
    ALIGNED_MEM4K(rbuf, ALIGNMENT4K);
    memset(rbuf, 0xff, ALIGNMENT4K);
    EXPECT_EQ((ssize_t)ALIGNMENT, reopened->pread(rbuf, ALIGNMENT, 0));
    for (size_t i = 0; i < ALIGNMENT; i++)
        EXPECT_EQ(0, rbuf[i]);
    delete reopened;
}

// yields inside every pwrite and records how many of them overlap, so that the photon
// threads writing a layer really interleave
class YieldingFile : public photon::fs::ForwardFile_Ownership {
public:
    int inflight = 0, max_inflight = 0;
    explicit YieldingFile(IFile *file) : ForwardFile_Ownership(file, true) {
    }
    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        max_inflight = max(max_inflight, ++inflight);
        photon::thread_yield();
        auto ret = m_file->pwrite(buf, count, offset);
        inflight--;
        return ret;
    }
};

TEST_F(HybridFileTest, restack_switches_data_file_mode) {
    auto lower_rw = create_file_rw();
    ASSERT_NE(nullptr, lower_rw);
    ALIGNED_MEM4K(lower_buf, ALIGNMENT4K);
    memset(lower_buf, 0x2a, ALIGNMENT4K);
    ASSERT_EQ((ssize_t)ALIGNMENT4K, lower_rw->pwrite(lower_buf, ALIGNMENT4K, 0));
    IFileRO *lower = nullptr;
    ASSERT_EQ(0, lower_rw->close_seal(&lower));
    delete lower_rw;

    auto upper = create_file_rw(RWType::Hybrid);
    ASSERT_NE(nullptr, upper);
    auto file = stack_files(upper, lower, true, false);
    ASSERT_NE(nullptr, file);
    ALIGNED_MEM4K(wbuf, ALIGNMENT4K);
    memset(wbuf, 0x4d, ALIGNMENT4K);
    EXPECT_EQ((ssize_t)ALIGNMENT4K, file->pwrite(wbuf, ALIGNMENT4K, ALIGNMENT4K));

    auto new_upper = create_file_rw(RWType::Hybrid);
    ASSERT_NE(nullptr, new_upper);
    ASSERT_EQ(0, new_upper->set_data_file_direct_io(true));
    auto new_data_name = data_name.back();
    EXPECT_EQ(0, file->restack(new_upper));
    ALIGNED_MEM4K(rbuf, ALIGNMENT4K);
    EXPECT_EQ((ssize_t)ALIGNMENT4K, file->pread(rbuf, ALIGNMENT4K, ALIGNMENT4K));
    EXPECT_EQ(0, memcmp(wbuf, rbuf, ALIGNMENT4K));
    auto new_data_size = file_size(lfs, new_data_name.c_str());
    EXPECT_EQ((ssize_t)ALIGNMENT, file->pwrite(wbuf, ALIGNMENT, 2 * ALIGNMENT4K));
    EXPECT_EQ(new_data_size + ALIGNMENT4K, file_size(lfs, new_data_name.c_str()));
    delete file;
}

// Hybrid writers hold no m_rw_mtx, so their data writes overlap, while an append-only
// layer still serializes them. Every thread writes its own disjoint 4K blocks.
TEST_F(HybridFileTest, concurrent_writes) {
    const int NTHREADS = 16;
    const uint32_t BLOCK = 4096 / ALIGNMENT;
    const int NWRITES = MAX_IMAGE_SIZE / ALIGNMENT / BLOCK / NTHREADS; // per thread
    const ssize_t INDEX_SIZE = HeaderTrailer::SPACE + NTHREADS * NWRITES * sizeof(SegmentMapping);

    auto concurrent_round = [&](IFileRW *file, unsigned char pattern) {
        vector<photon::join_handle *> threads;
        for (int t = 0; t < NTHREADS; t++) {
            auto th = photon::thread_create11([&, t] {
                for (int j = 0; j < NWRITES; j++)
                    do_io(file, {(uint64_t)(j * NTHREADS + t) * BLOCK, BLOCK,
                                 (unsigned char)(pattern + t)});
            });
            threads.push_back(photon::thread_enable_join(th));
        }
        for (auto th : threads)
            photon::thread_join(th);
    };

    for (auto type : {RWType::Append, RWType::Hybrid}) {
        LOG_INFO("concurrent writes, rw_type: `", rw_type_name(type));
        expected.assign(MAX_IMAGE_SIZE, 0);
        name_next_layer();
        auto fdata = new YieldingFile(
            lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU));
        auto findex = lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
        LayerInfo args(fdata, findex);
        args.rw_type = type;
        args.virtual_size = MAX_IMAGE_SIZE;
        auto file = ::create_file_rw(args, true);
        ASSERT_NE(nullptr, file);
        concurrent_round(file, 0x10);
        auto data_size = file_size(lfs, data_name.back().c_str());
        concurrent_round(file, 0x40);
        verify(file);
        auto max_inflight = fdata->max_inflight;
        delete file;
        if (type == RWType::Append) {
            EXPECT_EQ(1, max_inflight);
            continue;
        }
        EXPECT_GT(max_inflight, 1);
        // the first round appended every block exactly once, the second rewrote them
        EXPECT_EQ((ssize_t)(HeaderTrailer::SPACE + MAX_IMAGE_SIZE), data_size);
        EXPECT_EQ(data_size, file_size(lfs, data_name.back().c_str()));
        EXPECT_EQ(INDEX_SIZE, file_size(lfs, idx_name.back().c_str()));

        // the reopened layer resumes both append positions at the end of the files
        auto reopened = open_file_rw();
        ASSERT_NE(nullptr, reopened);
        verify(reopened);
        concurrent_round(reopened, 0x70);
        do_io(reopened, {0, BLOCK, /* discard = */ 0});
        do_io(reopened, {0, BLOCK, 0x7f});
        EXPECT_EQ(data_size + (ssize_t)(BLOCK * ALIGNMENT),
                  file_size(lfs, data_name.back().c_str()));
        EXPECT_EQ(INDEX_SIZE + (ssize_t)(2 * sizeof(SegmentMapping)),
                  file_size(lfs, idx_name.back().c_str()));
        verify(reopened);
        delete reopened;

        reopened = open_file_rw();
        ASSERT_NE(nullptr, reopened);
        verify(reopened);
        delete reopened;
    }
}

// The append positions and the group-commit buffer are shared by the writers of all
// the vCPUs serving a hybrid layer.
TEST_F(HybridFileTest, multi_vcpu_writes) {
    const uint64_t VSIZE = 4 << 20;
    const size_t BLOCK = 4096;
    const size_t NBLOCK = VSIZE / BLOCK;
    const int NVCPU = 4, NTHREADS = 4; // photon threads per vCPU
    const int NWRITERS = NVCPU * NTHREADS;

    // every block mixes its own number into its bytes, so neither a hole nor
    // another block passes for it
    auto byte_at = [](size_t b, size_t i, uint8_t v) {
        return (uint8_t)(b * 31 + ((i ^ (b * 13)) * 7) + v + 1);
    };

    name_next_layer();
    auto fdata = lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    auto findex = lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    ASSERT_NE(nullptr, fdata);
    ASSERT_NE(nullptr, findex);
    LayerInfo args(fdata, findex);
    args.rw_type = RWType::Hybrid;
    args.virtual_size = VSIZE;
    auto file = LSMT::create_file_rw(args, true);
    ASSERT_NE(nullptr, file);
    file->set_index_group_commit(4096);

    // writer w owns the blocks b with b % NWRITERS == w, the first round appends them,
    // the second rewrites them in place
    atomic<int> bad{0};
    auto vcpu_round = [&](uint8_t version) {
        vector<std::thread> vcpus;
        for (int v = 0; v < NVCPU; v++) {
            vcpus.emplace_back([&, v] {
                photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
                DEFER(photon::fini());
                vector<photon::join_handle *> threads;
                for (int t = 0; t < NTHREADS; t++) {
                    auto w = v * NTHREADS + t;
                    auto th = photon::thread_create11([&, w] {
                        ALIGNED_MEM4K(wbuf, BLOCK);
                        for (size_t b = w; b < NBLOCK; b += NWRITERS) {
                            for (size_t i = 0; i < BLOCK; i++)
                                wbuf[i] = byte_at(b, i, version);
                            if (file->pwrite(wbuf, BLOCK, (off_t)(b * BLOCK)) != (ssize_t)BLOCK)
                                bad.fetch_add(1);
                        }
                    });
                    threads.push_back(photon::thread_enable_join(th));
                }
                for (auto th : threads)
                    photon::thread_join(th);
            });
        }
        for (auto &t : vcpus)
            t.join();
    };
    auto verify_blocks = [&](IFileRO *f, uint8_t version) {
        size_t mismatch = 0;
        ALIGNED_MEM4K(rbuf, BLOCK);
        for (size_t b = 0; b < NBLOCK; b++) {
            if (f->pread(rbuf, BLOCK, (off_t)(b * BLOCK)) != (ssize_t)BLOCK) {
                mismatch++;
                continue;
            }
            for (size_t i = 0; i < BLOCK; i++) {
                if ((uint8_t)rbuf[i] != byte_at(b, i, version)) {
                    mismatch++;
                    break;
                }
            }
        }
        return mismatch;
    };

    vcpu_round(1);
    auto data_size = file_size(lfs, data_name.back().c_str());
    EXPECT_EQ((ssize_t)(HeaderTrailer::SPACE + VSIZE), data_size);
    vcpu_round(2);
    EXPECT_EQ(0, bad.load());
    EXPECT_EQ(0u, verify_blocks(file, 2));
    delete file;
    EXPECT_EQ(data_size, file_size(lfs, data_name.back().c_str()));

    auto reopened = open_file_rw();
    ASSERT_NE(nullptr, reopened);
    DEFER(delete reopened);
    EXPECT_EQ(0u, verify_blocks(reopened, 2));
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
    args.rw_type = RWType::Sparse;
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

// A hybrid RW layer must survive the same random workload as an append-only one, and
// grow slower, since the overwritten ranges are rewritten in place.
TEST_F(FileTest2, hybrid_rw) {
    reset_verify_file();
    auto append_layer = create_file_rw(RWType::Append);
    auto append_data = data_name.back();
    auto hybrid_layer = create_file_rw(RWType::Hybrid);
    auto hybrid_data = data_name.back();
    // the very same writes go to both layers, twice, so that the second round mostly
    // overwrites the ranges the writable layer already owns
    randwrite1(append_layer, hybrid_layer, FLAGS_nwrites);
    randwrite1(append_layer, hybrid_layer, FLAGS_nwrites);

    LOG_INFO("verify the append-only RW layer");
    EXPECT_TRUE(verify_file(append_layer));
    LOG_INFO("verify the hybrid RW layer");
    EXPECT_TRUE(verify_file(hybrid_layer));
    auto append_size = file_size(lfs, append_data.c_str());
    auto hybrid_size = file_size(lfs, hybrid_data.c_str());
    LOG_INFO("data file size { append: `, hybrid: ` }", append_size, hybrid_size);
    EXPECT_LT(hybrid_size, append_size);

    LOG_INFO("verify the committed hybrid RW layer");
    auto fcommit = lfs->open(layer_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    EXPECT_EQ(0, hybrid_layer->commit(fcommit));
    delete fcommit;
    verify_file(layer_name.back().c_str());
    delete append_layer;
    delete hybrid_layer;
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
    auto file1 = create_file_rw(RWType::Sparse);
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
    auto file = create_a_layer(RWType::Sparse);
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
                                       RWType::Sparse); //创建若干层RO Layer,文件指针保存在files中
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
    auto upper = create_a_layer(RWType::Sparse);

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

TEST_F(FileTest3, restack_simple) {
    CleanUp();
    auto lower = create_image(1);
    auto upper = create_a_layer();
    auto file = stack_files(upper, lower, 0, true);
    randwrite(file, FLAGS_nwrites);
    auto upper1 = create_file_rw();
    EXPECT_EQ(0, file->restack(upper1));
    cout << "restack top layer & verify" <<endl;
    verify_file(file);
    randwrite(file, FLAGS_nwrites);
    cout << "verify file after randwrite" <<endl;
    verify_file(file);
    delete file;
}

TEST_F(FileTest3, restack) {
    CleanUp();
    cout << "generating " << FLAGS_layers << " RO layers by randwrite()" << endl;
    auto lowers = create_image(FLAGS_layers);
    auto upper = create_a_layer();
    cout<<"stack files"<<endl;
    auto file = stack_files(upper, lowers, 0, true);
    randwrite(file, FLAGS_nwrites);
    verify_file(file);
    cout << "restack top layer 0" <<endl;
    auto upper1 = create_file_rw();
    EXPECT_EQ(0, file->restack(upper1));
    randwrite(file, FLAGS_nwrites);
    cout << "restack top layer 2 & verify" <<endl;
    auto upper2 = create_file_rw();
    EXPECT_EQ(0, file->restack(upper2));
    randwrite(file, FLAGS_nwrites);
    verify_file(file);
    delete file;
}
TEST_F(FileTest3, restack_sparse) {
    CleanUp();
    cout << "generating " << FLAGS_layers << " RO layers by randwrite()" << endl;
    auto lowers = create_image(FLAGS_layers);
    auto upper = create_a_layer(RWType::Sparse);
    cout<<"stack files"<<endl;
    auto file = stack_files(upper, lowers, 0, true);
    randwrite(file, FLAGS_nwrites);
    verify_file(file);
    cout << "restack top layer 0" <<endl;
    auto upper1 = create_file_rw(RWType::Sparse);
    EXPECT_EQ(0, file->restack(upper1));
    randwrite(file, FLAGS_nwrites);
    cout << "restack top layer 2 & verify" <<endl;
    auto upper2 = create_file_rw(RWType::Sparse);
    EXPECT_EQ(0, file->restack(upper2));
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

// The read-path counters belong to the file, not to the vCPU that reads it, so
// all the vCPUs serving the same image update them concurrently.
TEST_F(FileTest, multi_vcpu_io_counters) {
    const uint64_t VSIZE = 8 << 20;
    const size_t WLEN = 1 << 20; // length of each write, hence of each mapping
    const size_t BLOCK = 4096;   // length of each read
    const int NVCPU = 4;         // one OS thread (vCPU) each
    const int NREADS = 8192;     // reads per vCPU

    name_next_layer();
    auto fdata = lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    auto findex = lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    ASSERT_NE(nullptr, fdata);
    ASSERT_NE(nullptr, findex);
    LayerInfo args(fdata, findex);
    args.virtual_size = VSIZE;
    auto rw = LSMT::create_file_rw(args, true);
    ASSERT_NE(nullptr, rw);

    // fill the image, so that every read below hits exactly one mapping
    ALIGNED_MEM4K(wbuf, WLEN);
    memset(wbuf, 0xab, WLEN);
    for (uint64_t off = 0; off < VSIZE; off += WLEN) {
        ASSERT_EQ((ssize_t)WLEN, rw->pwrite(wbuf, WLEN, off));
    }
    ASSERT_EQ(0, rw->close_seal());
    delete rw;

    auto fro = lfs->open(data_name.back().c_str(), O_RDONLY);
    ASSERT_NE(nullptr, fro);
    auto file = dynamic_cast<LSMTReadOnlyFile *>(LSMT::open_file_ro(fro, true));
    ASSERT_NE(nullptr, file);
    DEFER(delete file);
    ASSERT_EQ(0u, file->lsmt_io_cnt.load());
    ASSERT_EQ(0u, file->lsmt_io_size.load());

    atomic<int> failed{0};
    vector<std::thread> vcpus;
    for (int i = 0; i < NVCPU; i++) {
        vcpus.emplace_back([&] {
            photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
            DEFER(photon::fini());
            ALIGNED_MEM4K(rbuf, BLOCK);
            for (int j = 0; j < NREADS; j++) {
                // keep every read within the first mapping: [0, WLEN)
                auto off = (off_t)((j % (WLEN / BLOCK)) * BLOCK);
                if (file->pread(rbuf, BLOCK, off) != (ssize_t)BLOCK)
                    failed.fetch_add(1);
            }
        });
    }
    for (auto &t : vcpus)
        t.join();

    EXPECT_EQ(0, failed.load());
    EXPECT_EQ((uint64_t)NVCPU * NREADS, (uint64_t)file->lsmt_io_cnt.load());
    EXPECT_EQ((uint64_t)NVCPU * NREADS * BLOCK, file->lsmt_io_size.load());
}

// A writable layer mutates its index in place, so without the index lock the
// readers of the other vCPUs walk a tree that is being rebalanced.
TEST_F(FileTest, multi_vcpu_concurrent_rw) {
    const uint64_t VSIZE = 1 << 20;
    const size_t BLOCK = 4096;
    const size_t NBLOCK = VSIZE / BLOCK;
    const int NWRITER = 2, NREADER = 2;
    const int NWRITES = 4000;  // per writer
    const int NREADS = 20000;  // per reader

    // every block mixes its own number into its bytes, so neither a hole nor
    // another block passes for it, and a block must agree on one version
    auto byte_at = [](size_t b, size_t i, uint8_t v) {
        return (uint8_t)(b * 31 + ((i ^ (b * 13)) * 7) + v + 1);
    };

    name_next_layer();
    auto fdata = lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    auto findex = lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    ASSERT_NE(nullptr, fdata);
    ASSERT_NE(nullptr, findex);
    LayerInfo args(fdata, findex);
    args.virtual_size = VSIZE;
    auto file = LSMT::create_file_rw(args, true);
    ASSERT_NE(nullptr, file);
    DEFER(delete file);

    // map every block first, so that no read below may legitimately hit a hole
    ALIGNED_MEM4K(wbuf, BLOCK);
    for (size_t b = 0; b < NBLOCK; b++) {
        for (size_t i = 0; i < BLOCK; i++)
            wbuf[i] = byte_at(b, i, 0);
        ASSERT_EQ((ssize_t)BLOCK, file->pwrite(wbuf, BLOCK, (off_t)(b * BLOCK)));
    }

    atomic<int> bad{0};
    vector<std::thread> vcpus;
    for (int w = 0; w < NWRITER; w++) {
        vcpus.emplace_back([&, w] {
            photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
            DEFER(photon::fini());
            ALIGNED_MEM4K(buf, BLOCK);
            uint32_t seed = 0x9e3779b9u * (w + 1);
            for (int j = 0; j < NWRITES; j++) {
                seed = seed * 1103515245 + 12345;
                auto b = (size_t)((seed >> 16) % NBLOCK);
                for (size_t i = 0; i < BLOCK; i++)
                    buf[i] = byte_at(b, i, (uint8_t)(j + 1));
                if (file->pwrite(buf, BLOCK, (off_t)(b * BLOCK)) != (ssize_t)BLOCK)
                    bad.fetch_add(1);
            }
        });
    }
    for (int r = 0; r < NREADER; r++) {
        vcpus.emplace_back([&, r] {
            photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
            DEFER(photon::fini());
            ALIGNED_MEM4K(buf, BLOCK);
            uint32_t seed = 0x85ebca6bu * (r + 1);
            for (int j = 0; j < NREADS; j++) {
                seed = seed * 1103515245 + 12345;
                auto b = (size_t)((seed >> 16) % NBLOCK);
                if (file->pread(buf, BLOCK, (off_t)(b * BLOCK)) != (ssize_t)BLOCK) {
                    bad.fetch_add(1);
                    continue;
                }
                auto v = (uint8_t)(((uint8_t *)buf)[0] - byte_at(b, 0, 0));
                for (size_t i = 0; i < BLOCK; i++) {
                    if (((uint8_t *)buf)[i] != byte_at(b, i, v)) {
                        bad.fetch_add(1);
                        break;
                    }
                }
            }
        });
    }
    for (auto &t : vcpus)
        t.join();

    EXPECT_EQ(0, bad.load());
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

class StagingTest : public FileTest {
public:
    struct FreeDeleter {
        void operator()(char *p) const {
            free(p);
        }
    };
    using Buffer = unique_ptr<char, FreeDeleter>;

    // the expected content of the virtual file
    vector<char> expected;

    static Buffer pattern(size_t size, uint32_t seed) {
        void *p = nullptr;
        EXPECT_EQ(0, posix_memalign(&p, ALIGNMENT4K, size));
        auto x = seed * 2654435761u + 1;
        for (size_t i = 0; i < size; i++) {
            x = x * 1103515245 + 12345;
            ((char *)p)[i] = x >> 16;
        }
        return Buffer((char *)p);
    }

    static RemoteMapping mapping(off_t offset, uint32_t count, off_t roffset) {
        RemoteMapping m;
        m.offset = offset;
        m.count = count;
        m.roffset = roffset;
        return m;
    }

    void expect(off_t offset, const char *data, size_t count) {
        if (expected.size() < offset + count)
            expected.resize(offset + count, 0);
        memcpy(&expected[offset], data, count);
    }

    // `data` is what the target holds at `roffset`
    void map(IFileRW *file, off_t offset, uint32_t count, off_t roffset, const char *data) {
        ASSERT_EQ((int)count, file->ioctl(IFileRW::RemoteData, mapping(offset, count, roffset)));
        expect(offset, data, count);
    }

    void write(IFileRW *file, off_t offset, const char *data, size_t count) {
        ASSERT_EQ((ssize_t)count, file->pwrite(data, count, offset));
        expect(offset, data, count);
    }

    static off_t append_payload(IFileRW *file, const char *data, size_t count) {
        auto upper = file->get_lower_files().back();
        auto pos = upper->lseek(0, SEEK_END);
        EXPECT_GE(pos, (off_t)HeaderTrailer::SPACE);
        EXPECT_EQ((ssize_t)count, upper->pwrite(data, count, pos));
        return pos;
    }

    void expect_rejected(IFileRW *file, off_t offset, uint32_t count, off_t roffset, int err) {
        errno = 0;
        EXPECT_EQ(-1, file->ioctl(IFileRW::RemoteData, mapping(offset, count, roffset)));
        EXPECT_EQ(err, errno);
    }

    void verify(IFileRO *file) {
        auto readback = pattern(expected.size(), 0);
        ASSERT_EQ((ssize_t)expected.size(), file->pread(readback.get(), expected.size(), 0));
        EXPECT_EQ(0, memcmp(expected.data(), readback.get(), expected.size()));
    }

    // the index of a sealed layer as it is stored in the layer
    vector<SegmentMapping> sealed_index(const char *fn, HeaderTrailer &trailer) {
        unique_ptr<IFile> file(lfs->open(fn, O_RDONLY));
        ALIGNED_MEM(buf, HeaderTrailer::SPACE, ALIGNMENT4K);
        auto pht = verify_ht(file.get(), buf, true);
        if (pht == nullptr) {
            ADD_FAILURE() << "no valid trailer in " << fn;
            return {};
        }
        trailer = *pht;
        vector<SegmentMapping> index(pht->index_size);
        auto bytes = index.size() * sizeof(SegmentMapping);
        EXPECT_EQ((ssize_t)bytes, file->pread(index.data(), bytes, pht->index_offset));
        return index;
    }
};

// staged data stays unmapped until RemoteData maps it, then survives reopen and close_seal()
TEST_F(StagingTest, stage_and_map) {
    auto file = create_file_rw();
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(0, file->can_map_staged_data());
    auto index_size = file_size(lfs, idx_name.back().c_str());

    // the first payload needs more than one mapping
    const size_t N1 = (Segment::MAX_LENGTH + 100) * ALIGNMENT, N2 = 64 << 10;
    auto a = pattern(N1, 1), b = pattern(N2, 2), c = pattern(32 << 10, 3);
    auto pa = append_payload(file, a.get(), N1);
    auto pb = append_payload(file, b.get(), N2);
    EXPECT_EQ((off_t)HeaderTrailer::SPACE, pa);
    EXPECT_EQ(pa + (off_t)N1, pb);
    EXPECT_EQ(pb + (ssize_t)N2, file_size(lfs, data_name.back().c_str()));
    EXPECT_EQ(0UL, file->index()->size());
    EXPECT_EQ(index_size, file_size(lfs, idx_name.back().c_str()));

    expect_rejected(file, (1 << 20) + 1, ALIGNMENT, pa, EINVAL);
    expect_rejected(file, 1 << 20, 100, pa, EINVAL);
    expect_rejected(file, 1 << 20, ALIGNMENT, pa + 1, EINVAL);
    expect_rejected(file, -(off_t)ALIGNMENT, ALIGNMENT, pa, EINVAL);
    expect_rejected(file, 0, ALIGNMENT, -(off_t)ALIGNMENT, EINVAL);
    expect_rejected(file, 0, ALIGNMENT, 0, EINVAL);
    expect_rejected(file, 0, ALIGNMENT, pa - ALIGNMENT, EINVAL);
    expect_rejected(file, 0, 2 * ALIGNMENT, pb + N2 - ALIGNMENT, EINVAL);
    expect_rejected(file, 0, ALIGNMENT, pb + N2, EINVAL);
    expect_rejected(file, 0, ALIGNMENT, INT64_MAX & ~(off_t)(ALIGNMENT - 1), EINVAL);
    EXPECT_EQ(pb + (ssize_t)N2, file_size(lfs, data_name.back().c_str()));
    EXPECT_EQ(0UL, file->index()->size());
    EXPECT_EQ(index_size, file_size(lfs, idx_name.back().c_str()));

    map(file, 1 << 20, N1, pa, a.get());
    EXPECT_EQ(2UL, file->index()->size());
    map(file, 0, N2, pb, b.get());
    // overrides a part of the first payload
    map(file, (1 << 20) + 4096, 8192, pb, b.get());
    EXPECT_EQ(index_size + 4 * (ssize_t)sizeof(SegmentMapping),
              file_size(lfs, idx_name.back().c_str()));

    write(file, 12 << 20, c.get(), 32 << 10);
    SegmentMapping written;
    ASSERT_EQ(1UL, file->index()->lookup(Segment{(12 << 20) / ALIGNMENT, 1}, &written, 1));
    EXPECT_EQ((uint64_t)(pb + N2), written.moffset * ALIGNMENT);
    map(file, 24 << 20, ALIGNMENT, pb + N2, c.get());
    verify(file);
    delete file;

    file = open_file_rw();
    ASSERT_NE(nullptr, file);
    verify(file);
    ASSERT_EQ(0, file->can_map_staged_data());
    map(file, 0, ALIGNMENT, pa, a.get());
    auto d = pattern(4096, 4);
    auto pd = append_payload(file, d.get(), 4096);
    EXPECT_EQ(pb + (off_t)N2 + (32 << 10), pd);
    map(file, 20 << 20, 4096, pd, d.get());
    verify(file);
    auto data_end = file_size(lfs, data_name.back().c_str());
    EXPECT_EQ(pd + 4096, data_end);

    IFileRO *sealed = nullptr;
    ASSERT_EQ(0, file->close_seal(&sealed));
    delete file;
    ASSERT_NE(nullptr, sealed);
    verify(sealed);
    delete sealed;

    // close_seal() appends the index and the trailer after the data in place
    HeaderTrailer ht;
    auto index = sealed_index(data_name.back().c_str(), ht);
    EXPECT_EQ((uint64_t)data_end, ht.index_offset);
    auto index_bytes = (index.size() * sizeof(SegmentMapping) + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT;
    EXPECT_EQ(data_end + (ssize_t)(index_bytes + HeaderTrailer::SPACE),
              file_size(lfs, data_name.back().c_str()));
    uint64_t end = 0;
    for (auto &m : index) {
        EXPECT_EQ(0, (int)m.tag);
        EXPECT_FALSE(m.zeroed);
        EXPECT_LE(end, (uint64_t)m.offset);
        end = m.end();
        EXPECT_LE((uint64_t)pa, m.moffset * ALIGNMENT);
        EXPECT_LE(m.mend() * ALIGNMENT, (uint64_t)data_end);
    }

    auto ro = open_file_ro(data_name.back().c_str());
    ASSERT_NE(nullptr, ro);
    verify(ro);
    delete ro;
}

// staging into a layer stacked on a sealed lower layer only touches the upper layer
TEST_F(StagingTest, stacked_lower) {
    auto lower_rw = create_file_rw();
    ASSERT_NE(nullptr, lower_rw);
    auto l = pattern(256 << 10, 5);
    write(lower_rw, 0, l.get(), 256 << 10);
    IFileRO *lower = nullptr;
    ASSERT_EQ(0, lower_rw->close_seal(&lower));
    delete lower_rw;
    auto lower_size = file_size(lfs, data_name.front().c_str());

    auto upper = create_file_rw();
    ASSERT_NE(nullptr, upper);
    auto file = stack_files(upper, lower, /* ownership = */ true, /* check_order = */ false);
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(0, file->can_map_staged_data());
    auto u = pattern(64 << 10, 6);
    auto pu = append_payload(file, u.get(), 64 << 10);
    EXPECT_EQ((off_t)HeaderTrailer::SPACE, pu);
    map(file, 64 << 10, 32 << 10, pu + (32 << 10), u.get() + (32 << 10));
    verify(file);
    EXPECT_EQ(lower_size, file_size(lfs, data_name.front().c_str()));
    EXPECT_EQ(pu + (64 << 10), file_size(lfs, data_name.back().c_str()));
    delete file;

    // seal the upper layer alone, as `overlaybd-commit --seal` does
    file = open_file_rw();
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(0, file->close_seal());
    delete file;
    HeaderTrailer ht;
    auto index = sealed_index(data_name.back().c_str(), ht);
    ASSERT_EQ(1UL, index.size());
    EXPECT_EQ(128UL, (uint64_t)index[0].offset);
    EXPECT_EQ(64U, (uint32_t)index[0].length);
    EXPECT_EQ((uint64_t)(pu + (32 << 10)) / ALIGNMENT, (uint64_t)index[0].moffset);
    EXPECT_EQ(0, (int)index[0].tag);

    IFile *files[] = {lfs->open(data_name.front().c_str(), O_RDONLY),
                      lfs->open(data_name.back().c_str(), O_RDONLY)};
    auto image = open_files_ro(files, 2, true);
    ASSERT_NE(nullptr, image);
    verify(image);
    delete image;
}

// close_seal() merges the mappings that are contiguous both in the virtual file and in the layer
TEST_F(StagingTest, close_seal_merges_mappings) {
    auto file = create_file_rw();
    ASSERT_NE(nullptr, file);
    const uint32_t N = 64 << 10;
    auto a = pattern(3 * N, 8), w = pattern(2 * N, 9);
    auto pa = append_payload(file, a.get(), 3 * N);
    for (uint32_t i = 0; i < 3; i++)
        map(file, (1 << 20) + i * N, N, pa + i * N, a.get() + i * N);
    // logically adjacent to the payload, but not contiguous with it in the layer
    map(file, (1 << 20) + 3 * N, N, pa, a.get());
    // contiguous with the payload in the layer, but not logically adjacent to it
    write(file, 8 << 20, w.get(), N);
    write(file, (8 << 20) + N, w.get() + N, N);
    ASSERT_EQ(0, file->fallocate(3, 12 << 20, N));
    ASSERT_EQ(0, file->fallocate(3, (12 << 20) + N, N));
    vector<char> zeros(2 * N, 0);
    expect(12 << 20, zeros.data(), 2 * N);
    EXPECT_EQ(8UL, file->index()->size());

    IFileRO *sealed = nullptr;
    ASSERT_EQ(0, file->close_seal(&sealed));
    delete file;
    ASSERT_NE(nullptr, sealed);
    EXPECT_EQ(4UL, sealed->index()->size());
    verify(sealed);
    delete sealed;

    HeaderTrailer ht;
    auto index = sealed_index(data_name.back().c_str(), ht);
    ASSERT_EQ(4UL, index.size());
    auto expect_mapping = [](const SegmentMapping &m, off_t offset, uint32_t length,
                             off_t moffset, bool zeroed) {
        EXPECT_EQ((uint64_t)offset / ALIGNMENT, (uint64_t)m.offset);
        EXPECT_EQ(length / ALIGNMENT, (uint32_t)m.length);
        EXPECT_EQ((uint64_t)moffset / ALIGNMENT, (uint64_t)m.moffset);
        EXPECT_EQ(zeroed, (bool)m.zeroed);
    };
    expect_mapping(index[0], 1 << 20, 3 * N, pa, false);
    expect_mapping(index[1], (1 << 20) + 3 * N, N, pa, false);
    expect_mapping(index[2], 8 << 20, 2 * N, pa + 3 * N, false);
    expect_mapping(index[3], 12 << 20, 2 * N, pa + 5 * N, true);
    // the rest of the last index block maps nothing
    vector<SegmentMapping> padding(ALIGNMENT / sizeof(SegmentMapping) - index.size());
    unique_ptr<IFile> fdata(lfs->open(data_name.back().c_str(), O_RDONLY));
    auto padding_bytes = padding.size() * sizeof(SegmentMapping);
    ASSERT_EQ((ssize_t)padding_bytes,
              fdata->pread(padding.data(), padding_bytes,
                           ht.index_offset + index.size() * sizeof(SegmentMapping)));
    for (auto &m : padding)
        EXPECT_EQ((uint64_t)SegmentMapping::INVALID_OFFSET, (uint64_t)m.offset);

    auto ro = open_file_ro(data_name.back().c_str());
    ASSERT_NE(nullptr, ro);
    EXPECT_EQ(4UL, ro->index()->size());
    verify(ro);
    delete ro;
}

// only a plain append-mode layer maps embedded payload, while warp keeps external RemoteData
TEST_F(StagingTest, other_layer_types) {
    auto data = pattern(64 << 10, 7);
    for (auto rw_type : {RWType::Sparse, RWType::Hybrid}) {
        auto file = create_file_rw(rw_type);
        ASSERT_NE(nullptr, file);
        DEFER(delete file);
        auto data_size = file_size(lfs, data_name.back().c_str());
        // the sparse layer leaves it unimplemented, the hybrid one does not support it
        auto err = rw_type == RWType::Sparse ? ENOSYS : ENOTSUP;
        errno = 0;
        EXPECT_EQ(-1, file->can_map_staged_data());
        EXPECT_EQ(err, errno);
        expect_rejected(file, 0, 4096, HeaderTrailer::SPACE, err);
        EXPECT_EQ(data_size, file_size(lfs, data_name.back().c_str()));
        EXPECT_EQ(0UL, file->index()->size());
    }

    name_next_layer();
    auto ftarget = lfs->open(layer_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    ASSERT_NE(nullptr, ftarget);
    ASSERT_EQ((ssize_t)(64 << 10), ftarget->pwrite(data.get(), 64 << 10, 0));
    auto fmeta = lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    auto findex = lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
    WarpFileArgs args(findex, fmeta, ftarget);
    args.virtual_size = vsize;
    auto warp = create_warpfile(args, true);
    ASSERT_NE(nullptr, warp);
    DEFER(delete warp);
    errno = 0;
    EXPECT_EQ(-1, warp->can_map_staged_data());
    EXPECT_EQ(ENOSYS, errno);
    map(warp, 1 << 20, 32 << 10, 16 << 10, data.get() + (16 << 10));
    verify(warp);
    // unaligned mappings are rejected rather than truncated
    expect_rejected(warp, 0, 100, 0, EINVAL);
    expect_rejected(warp, 0, ALIGNMENT, 1, EINVAL);
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
