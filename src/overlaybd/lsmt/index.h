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

#pragma once
#include <inttypes.h>
#include <cstddef>
#include <assert.h>
#include <sys/types.h>

namespace LSMT {
struct Segment {          // 48 + 18 == 64
    uint64_t offset : 50; // offset (0.5 PB if in sector)
    uint32_t length : 14; // length (8MB if in sector)
    const static uint64_t MAX_OFFSET = (1UL << 50) - 1;
    const static uint32_t MAX_LENGTH = (1 << 14) - 1;
    const static uint64_t INVALID_OFFSET = MAX_OFFSET;
    uint64_t end() const {
        return offset + length;
    }
    uint64_t forward_offset_to(uint64_t x) {
        assert(x >= offset);
        auto delta = x - offset;
        length -= delta;
        offset = x;
        return delta;
    }
    void backward_end_to(uint64_t x) {
        assert(x > offset);
        length = (uint32_t)(x - offset);
    }
} __attribute__((packed));

struct SegmentMapping : public Segment { // 64 + 55 + 9 == 128
    uint64_t moffset : 55;               // mapped offset (2^64 B if in sector)
    uint32_t zeroed : 1;                 // indicating a zero-filled segment
    uint8_t tag;
    const static uint64_t MAX_MOFFSET = (1UL << 55) - 1;

    SegmentMapping() {
    }
    SegmentMapping(uint64_t loffset, uint32_t length, uint64_t moffset, uint8_t tag = 0)
        : Segment{loffset, length}, moffset(moffset), zeroed(0), tag(tag) {
        assert(length <= Segment::MAX_LENGTH);
    }

    uint64_t mend() const {
        return (zeroed ? moffset : moffset + length);
    }
    void forward_offset_to(uint64_t x) {
        auto delta = Segment::forward_offset_to(x);
        moffset += (!zeroed ? delta : 0);
    }
    void backward_end_to(uint64_t x) {
        assert(x > offset);
        length = (uint32_t)(x - offset);
    }
    SegmentMapping &discard() {
        zeroed = 1;
        return *this;
    }
    static SegmentMapping invalid_mapping() {
        return SegmentMapping(INVALID_OFFSET, 0, 0);
    }
} __attribute__((packed));

struct RemoteMapping {
    off_t offset;
    uint32_t count;
    off_t roffset;
};

enum class SegmentType {
    fsMeta,
    remoteData,
};

// a read-only memory index for log-structured data
class IMemoryIndex {
public:
    virtual ~IMemoryIndex() {
    }

    // number of segments in the index
    virtual size_t size() const = 0;

    // the raw buffer (an array of SegmentMapping) used by memory index
    virtual const SegmentMapping *buffer() const = 0;

    // look up mappings within the segment `s` in logical space,
    // stores found mappings in pm[0..n], returning # of stored mappings;
    // edge (the first and the last) mappings will be trimmed by `s`.
    virtual size_t lookup(Segment s, /* OUT */ SegmentMapping *pm, size_t n) const = 0;

    template <size_t N>
    size_t lookup(Segment s, SegmentMapping (&pm)[N]) const {
        return lookup(s, pm, N);
    }

    // returns the first and last mapping in the index
    // the there's no one, return an invalid mapping: [INVALID_OFFSET, 0) ==> 0
    virtual SegmentMapping front() const = 0;
    virtual SegmentMapping back() const = 0;

    virtual int increase_tag(int delta = 1) = 0;

    // number of 512B blocks allocated
    virtual uint64_t block_count() const = 0;

    virtual uint64_t vsize() const = 0;

    virtual IMemoryIndex *make_read_only_index() const = 0;
};

// the level 0 memory index, which supports write
class IMemoryIndex0 : public IMemoryIndex {
public:
    // insert a new segment mapping to the index
    virtual void insert(SegmentMapping m) = 0;

    // dump the the whole index as an array
    // memory allocation is aligned to the `alignment`
    virtual SegmentMapping *dump(size_t alignment = 0) const = 0;
    // virtual IMemoryIndex *make_read_only_index() const = 0;
};

class IComboIndex : public IMemoryIndex0 {
public:
    // backing index must NOT be IMemoryIndex0!
    virtual int backing_index(const IMemoryIndex *bi) = 0;
    virtual const IMemoryIndex *backing_index() const = 0;
    virtual const IMemoryIndex0 *front_index() const = 0;

    // dump index0 which needs to compact
    // and then clear the original index0.
    // virtual IMemoryIndex0* gc_index() = 0;
    virtual IMemoryIndex *load_range_index(int, int) const = 0;


};

// create writable level 0 memory index from an array of mappings;
// the array may be freed immediately after the function returns;
// the mapped offset must be within [moffset_begin, moffset_end)
extern "C" IMemoryIndex0 *create_memory_index0(const SegmentMapping *pmappings, std::size_t n,
                                               uint64_t moffset_begin, uint64_t moffset_end);
inline IMemoryIndex0 *create_memory_index0() {
    return create_memory_index0(nullptr, 0, 0, UINT64_MAX);
}

// create a read-only memory index from an array of mappings;
// the mappings should have been sorted and should not intersect with each other!!
// the array buffer must remain valid as long as the index is valid, if copy_mode = 0 or 1.
//     in addition, the array buffer will delete in ~IMemoryIndex() if copy_mode = 1.
// the mapped offset must be within [moffset_begin, moffset_end)
extern "C" IMemoryIndex *create_memory_index(const SegmentMapping *pmappings, std::size_t n,
                                             uint64_t moffset_begin, uint64_t moffset_end,
                                             bool ownership = true, uint64_t vsize = 0);

// merge multiple indexes into a single one index
// the `tag` field of each element in the result is subscript of `pindexes`:
// after creation, the sources can be safely destoryed
extern "C" IMemoryIndex *merge_memory_indexes(const IMemoryIndex **pindexes, std::size_t n);

// combine an index0 and an index into a combo, which, when looked-up, behaves as if they
// were one single index; inserting into a combo effectively inserting into the index0 part;
// the mapped offset must be within [moffset_begin, moffset_end)
extern "C" IComboIndex *create_combo_index(IMemoryIndex0 *index0, const IMemoryIndex *index,
                                           uint8_t ro_index_count, bool ownership);

// compress raw index array by mergeing adjacent continuous mappings
// returning compressed size of the array
extern "C" std::size_t compress_raw_index(SegmentMapping *mapping, std::size_t n);
extern "C" std::size_t compress_raw_index_predict(const SegmentMapping *mapping, std::size_t n);

// lookup `idx` with `s`, and visit each segments via callbacks
template <typename CB1, typename CB2>
inline int foreach_segments(IMemoryIndex *idx, Segment s, CB1 cb_zero, CB2 cb_data) {
    const size_t NMAPPING = 16;
    SegmentMapping mappings[NMAPPING];
    while (true) {
        auto n = idx->lookup(s, mappings, NMAPPING);
        for (size_t i = 0; i < n; ++i) {
            auto &m = mappings[i];
            if (s.offset < m.offset) {
                Segment mm{s.offset, (uint32_t)(m.offset - s.offset)};
                // hole
                int ret = cb_zero(mm);
                if (ret < 0)
                    return ret;
            }
            // zeroe block
            int ret = (m.zeroed) ? cb_zero(m) : cb_data(m);
            if (ret < 0)
                return ret;

            s.forward_offset_to(m.end());
        }
        if (n < NMAPPING)
            break;
    }
    if (s.length > 0)
        cb_zero(s);
    return 0;
}
} // namespace LSMT
