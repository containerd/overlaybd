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

#include "index.h"
#include <vector>
#include <set>
#include <algorithm>
#include <memory>
#include <photon/common/alog.h>
#include <photon/fs/filesystem.h>
#include <photon/common/utility.h>
using namespace std;

namespace LSMT {
static inline bool operator<(const Segment &a, const Segment &b) {
    return a.end() <= b.offset; // because end() is not inclusive
}
static void trim_edge_mappings(LSMT::SegmentMapping *pm, size_t m, Segment s) {
    if (m == 0)
        return;
    if (pm[0].offset < s.offset)
        pm[0].forward_offset_to(s.offset);

    // back may be pm[0], when m == 1
    auto &back = pm[m - 1];
    if (back.end() > s.end())
        back.backward_end_to(s.end());
}
template <class IT>
static inline size_t copy_n(IT begin, IT end, uint64_t end_offset, SegmentMapping *pm, size_t n) {
    size_t m = 0;
    for (auto it = begin; it != end; ++it) {
        if (it->offset >= end_offset)
            break;
        pm[m++] = *it;
        if (m == n)
            break;
    }
    return m;
}

static bool verify_mapping_order(const SegmentMapping *pmappings, size_t n);

class Index : public IMemoryIndex {
public:
    bool ownership = false;
    vector<SegmentMapping> mapping;
    const SegmentMapping *pbegin = nullptr;
    const SegmentMapping *pend = nullptr;
    uint64_t alloc_blk = 0;
    uint64_t virtual_size = 0;

    inline void get_alloc_blks() {
        for (auto m : mapping) {
            alloc_blk += m.length * (!m.zeroed);
        }
    }
    ~Index() {
        if (ownership) {
            delete[] pbegin;
        }
    }
    Index(const SegmentMapping *pmappings = nullptr, size_t n = 0, bool ownership = true,
          uint64_t vsize = 0)
        : ownership(ownership), virtual_size(vsize) {
        if (n == 0 || pmappings == nullptr) {
            pbegin = pend = nullptr;
            return;
        }
        pbegin = pmappings;
        pend = pbegin + n;
    }
    Index(vector<SegmentMapping> &&m, uint64_t vsize = 0)
        : mapping(std::move(m)), virtual_size(vsize) {
        if (mapping.size()) {
            pbegin = &mapping[0];
            pend = pbegin + mapping.size();
            get_alloc_blks();
        } else
            pbegin = pend = nullptr;
    }

    virtual uint64_t block_count() const override {
        return alloc_blk;
    }

    template <typename IT>
    void assign(IT begin, IT end) {
        mapping.assign(begin, end);
        pbegin = &mapping[0];
        pend = pbegin + mapping.size();
        get_alloc_blks();
    }
    // number of segments in the index
    virtual size_t size() const override {
        return pend - pbegin;
    }

    // the raw buffer (an array of SegmentMapping) used by memory index
    virtual const SegmentMapping *buffer() const override {
        return pbegin;
    }

    // look up mappings within the segment `s` in logical space,
    // stores found mappings in pm[0..n], returning # of stored mappings;
    // edge (the first and the last) mappings will be trimmed by `s`.
    virtual size_t lookup(Segment s, /* OUT */ SegmentMapping *pm, size_t n) const override {
        if (s.length == 0)
            return 0;
        auto lb = std::lower_bound(pbegin, pend, s);
        auto m = copy_n(lb, pend, s.end(), pm, n);
        trim_edge_mappings(pm, m, s);
        return m;
    }

    virtual SegmentMapping front() const override {
        return (pbegin != pend) ? *pbegin : SegmentMapping::invalid_mapping();
    }
    virtual SegmentMapping back() const override {
        return (pbegin != pend) ? *(pend - 1) : SegmentMapping::invalid_mapping();
    }
    const SegmentMapping *lower_bound(uint64_t offset) const {
        return std::lower_bound(pbegin, pend, Segment{offset, 1});
    }
    const SegmentMapping *begin() const {
        return pbegin;
    }
    const SegmentMapping *end() const {
        return pend;
    }

    int increase_tag(int delta) override {
        LOG_DEBUG("index tag add `", delta);
        auto array = ptr_array((SegmentMapping *)this->buffer(), this->size());
        for (auto &m : array)
            m.tag += delta;
        return 0;
    }

    uint64_t vsize() const override {
        return virtual_size;
    }

    UNIMPLEMENTED_POINTER(IMemoryIndex  *make_read_only_index() const override);
};

class LevelIndex : public Index {
public:
    vector<vector<uint64_t>> level_mapping;
    static const uint16_t LEVEL_LSHIFT = 9;
    static const uint16_t PAGE_SIZE = (1 << LEVEL_LSHIFT) * sizeof(uint64_t);
    // LSHIFT = 6 --> PAGE_SIZE = 256, LSHIFT = 9 --> PAGE_SIZE = 4096
    LevelIndex(const SegmentMapping *pmappings = nullptr, size_t n = 0, bool ownership = true)
        : Index(pmappings, n, ownership) {
        build_level_index((uint8_t *)pbegin, (uint8_t *)pend, sizeof(SegmentMapping), 0);
        print_info();
    }
    LevelIndex(vector<SegmentMapping> &&m) : Index(std::move(m)) {
        build_level_index((uint8_t *)pbegin, (uint8_t *)pend, sizeof(SegmentMapping), 0);
        print_info();
    }

    void print_info() {
        char msg[256]{};
        char *cur = msg;
        for (auto it = level_mapping.begin(); it != level_mapping.end(); it++) {
            cur += sprintf(cur, " %zu ", (*it).size());
        }
        LOG_INFO("create level index, depth: `, elements # per level {` `}", level_mapping.size(),
                 msg, this->size());
    }

    void build_level_index(uint8_t *begin, uint8_t *end, size_t obj_size, int depth) {
        if (begin == nullptr)
            return;
        int page_size = PAGE_SIZE / obj_size;
        auto n = (end - begin) / obj_size;
        LOG_DEBUG("level ` offset size(`)", depth, (n - 1) / page_size + 1);
        auto extent_size = (n - 1) / page_size + 1;
        vector<uint64_t> extent(extent_size);
        DEFER(level_mapping.push_back(extent));
        int p = 0;
        for (auto ptr = begin; ptr < end; ptr += page_size * obj_size) {
            if (depth == 0) {
                extent[p++] = ((SegmentMapping *)ptr)->offset;
                continue;
            }
            extent[p++] = *(uint64_t *)ptr;
        }
        if (extent_size > PAGE_SIZE / sizeof(uint64_t)) {
            build_level_index((uint8_t *)&extent[0], (uint8_t *)(&extent[0] + extent_size),
                              sizeof(uint64_t), depth + 1);
        }
    }

    virtual size_t lookup(Segment s, /* OUT */ SegmentMapping *pm, size_t n) const override {
        if (s.length == 0 || level_mapping.empty())
            return 0;
        const LSMT::SegmentMapping *lb = pbegin;
        int lower = 0, upper = level_mapping[0].size();
        int page_offset = 0;
        for (int i = 0; i < (int)level_mapping.size(); i++) {
            auto extent = &(level_mapping[i][0]);
            auto page_lb = std::lower_bound(extent + lower, extent + upper, s.offset);
            page_offset = upper;
            if (page_lb != extent + upper) {
                page_offset = page_lb - &extent[0];
            }
            if (page_offset == 0)
                break;
            bool bottom = (i == (int)level_mapping.size() - 1);
            int lshift = LEVEL_LSHIFT - (int)bottom;
            int underlay_size = (bottom ? this->size() : level_mapping[i + 1].size());
            lower = (page_offset - 1) << lshift;
            upper = min((page_offset) << lshift, (int)underlay_size);
        }
        LOG_DEBUG("{page_offset: `}", page_offset);
        if (page_offset) {
            lb = std::lower_bound(pbegin + lower, pbegin + upper, s);
        }
        auto m = copy_n(lb, pend, s.end(), pm, n);
        trim_edge_mappings(pm, m, s);
        return m;
    }
};

class Index0 : public IComboIndex {
public:
    set<SegmentMapping> mapping;
    typedef set<SegmentMapping>::iterator iterator;

    struct block_usage {
        uint64_t m_alloc = 0;
        inline void operator-=(const SegmentMapping &m) {
            if (m.zeroed)
                return;
            m_alloc = m_alloc - m.length;
        }
        inline void operator+=(const SegmentMapping &m) {
            if (m.zeroed)
                return;
            m_alloc = m_alloc + m.length;
        }
    } alloc_blk;

    // Index0(const set<SegmentMapping> &mapping) : mapping(mapping){};

    Index0(const SegmentMapping *pmappings = nullptr, size_t n = 0) {
        if (pmappings == nullptr)
            return;
        for (size_t i = 0; i < n; ++i)
            insert(pmappings[i]);
    }
    // number of segments in the index
    virtual size_t size() const override {
        return mapping.size();
    }
    // the raw buffer (an array of SegmentMapping) used by memory index
    virtual const SegmentMapping *buffer() const override {
        return nullptr;
    }
    iterator remove_partial_overlap(iterator it, uint64_t offset, uint32_t length) {
        auto nx = next(it);
        auto end = offset + length;
        auto p = (SegmentMapping *)&*it;
        if (p->offset < offset) // p->offset < offset < p->end() < end
        {
            assert(p->end() > offset);
            alloc_blk -= *p;
            if (p->end() <= end) {
                p->backward_end_to(offset);
                alloc_blk += *p;
            } else // if (p->end() > end) // m lies in *p
            {
                SegmentMapping nm = *p;
                nm.forward_offset_to(end);
                p->backward_end_to(offset); // shrink first,
                mapping.insert(it, nm);     // and then insert() !!!
                alloc_blk += *p;
                alloc_blk += nm;
            }
        } else if (/* p->offset >= m.offset && */ p->offset < end) {
            alloc_blk -= *p;
            if (p->end() <= end) // included by [offset, end)
            {
                mapping.erase(it);
            } else // (p->end() > end)
            {
                p->forward_offset_to(end);
                alloc_blk += *p;
            }
        }
        return nx;
    }
    iterator prev(iterator it) const {
        return --it;
    }
    iterator next(iterator it) const {
        return ++it;
    }
    virtual void insert(SegmentMapping m) override {
        if (m.length == 0)
            return;
        alloc_blk += m;
        auto it = mapping.lower_bound(m);
        if (it == mapping.end()) {
            mapping.insert(m);
            return;
        }

        it = remove_partial_overlap(it, m.offset, m.length); // first one (there must be)
        assert(it == mapping.end() || it->offset > m.offset);
        while (it != mapping.end() && it->offset < m.end()) {
            if (it->end() <= m.end()) {
                alloc_blk -= *it;
                it = mapping.erase(it); // middle ones, if there are
            } else {
                it = remove_partial_overlap(it, m.offset, m.length); // last one, if there is
                break;
            }
        }
        mapping.insert(it, m);
    }

    virtual size_t lookup(Segment s, /* OUT */ SegmentMapping *pm, size_t n) const override {
        if (s.length == 0)
            return 0;
        SegmentMapping ss(s.offset, s.length, 0);
        auto lb = mapping.lower_bound(ss);
        auto m = copy_n(lb, mapping.end(), s.end(), pm, n);
        trim_edge_mappings(pm, m, s);
        return m;
    }

    // dump the the whole index as an array
    virtual SegmentMapping *dump(size_t alignment = 0) const override {
        auto size = mapping.size();
        if (alignment > 0) {
            alignment /= sizeof(SegmentMapping);
            size = (size + alignment - 1) / alignment * alignment;
        }
        LOG_INFO("index dump, size: ` ( mapping.size: ` )", size, mapping.size());
        auto rst = new SegmentMapping[size];
        std::copy(mapping.begin(), mapping.end(), rst);
        return rst;
    }

    virtual IMemoryIndex *make_read_only_index() const override {
        auto rst = new Index();
        rst->mapping.reserve(size());
        rst->assign(mapping.begin(), mapping.end());
        return rst;
    }

    virtual uint64_t block_count() const override {
        return alloc_blk.m_alloc;
    }

    // returns the first and last mapping in the index
    // the there's no one, return an invalid mapping: [INVALID_OFFSET, 0) ==> 0
    virtual SegmentMapping front() const override {
        return !mapping.empty() ? *mapping.begin() : SegmentMapping::invalid_mapping();
    }
    virtual SegmentMapping back() const override {
        printf("!mapping.empty()? :%d\n", mapping.empty());
        return !mapping.empty() ? *prev(mapping.end()) : SegmentMapping::invalid_mapping();
    }

    iterator lower_bound(uint64_t offset) const {
        SegmentMapping ss(offset, 1, 0);
        return mapping.lower_bound(ss);
    }
    iterator end() const {
        return mapping.end();
    }

    UNIMPLEMENTED(int backing_index(const IMemoryIndex *bi) override);
    UNIMPLEMENTED(int increase_tag(int) override);

    UNIMPLEMENTED_POINTER(IMemoryIndex *load_range_index(int, int) const override);

    UNIMPLEMENTED_POINTER(const IMemoryIndex *backing_index() const override);
    virtual const IMemoryIndex0 *front_index() const override {
        return this;
    }
    UNIMPLEMENTED(size_t vsize() const override);
};

static void merge_indexes(uint8_t level, vector<SegmentMapping> &mapping, const Index **pindexes,
                          std::size_t n, uint64_t begin, uint64_t end, bool change_tag = true,
                          size_t max_level = 0);

class ComboIndex : public Index0 {
public:
    Index0 *m_index0{nullptr};
    Index *m_backing_index{nullptr};
    bool m_ownership;

    ComboIndex(Index0 *index0, const Index *index, uint8_t ro_layers_count, bool ownership) {
        m_index0 = index0;
        m_backing_index = const_cast<Index *>(index);
        mapping = index0->mapping;
        m_ownership = ownership;

        for (auto &x : mapping)
            ((SegmentMapping &)x).tag += ro_layers_count;
        // for (auto &x : *m_backing_index)
        //     ((SegmentMapping &)x).tag++;
    }
    ~ComboIndex() {
        if (m_ownership) {
            delete m_index0;
            delete m_backing_index;
        }
    }

    virtual const IMemoryIndex0 *front_index() const override {
        return this->m_index0;
    }

    virtual size_t lookup(Segment s, /* OUT */ SegmentMapping *pm, size_t n) const override {
        if (s.length == 0)
            return 0;
        if (!m_backing_index)
            return Index0::lookup(s, pm, n);

        auto pm_ = pm;
        auto it = mapping.lower_bound({s.offset, s.length, 0});
        auto soffset = s.offset;
        auto send = s.end();
        while (it != mapping.end() && it->offset < send && n) {
            if (it->offset > soffset) {
                auto s1 = Segment{soffset, (uint32_t)(it->offset - soffset)};
                auto m1 = m_backing_index->lookup(s1, pm, n);
                pm += m1;
                n -= m1;
                if (n == 0)
                    break;
            }
            soffset = it->end();
            *pm++ = *it++;
            n--;
        }
        if (n && soffset < send) {
            auto s1 = Segment{soffset, (uint32_t)(send - soffset)};
            auto m1 = m_backing_index->lookup(s1, pm, n);
            pm += m1;
            n -= m1;
        }
        auto m = pm - pm_;
        trim_edge_mappings(pm_, m, s);
        return m;
    }

    virtual int backing_index(const IMemoryIndex *bi) override {
        if (!bi || !bi->buffer()) {
            errno = EINVAL;
            LOG_ERROR("combo index can NOT be created with IMemoryIndex0!");
            return -1;
        }
        if (m_backing_index != nullptr) {
            delete m_backing_index;
            m_backing_index = nullptr;
        }
        m_backing_index = (Index *)bi;
        return 0;
    }

    virtual const IMemoryIndex *backing_index() const override {
        return m_backing_index;
    }

    virtual IMemoryIndex *load_range_index(int min_level, int max_level) const override {
        if (min_level >= max_level) {
            return nullptr;
        }
        LOG_DEBUG("` <= m.tag <= `", min_level, max_level - 1);
        vector<SegmentMapping> range_index{};
        auto index = m_backing_index->buffer();
        for (auto m : ptr_array(index, m_backing_index->size())) {
            if (((int)m.tag) >= min_level && ((int)m.tag) < max_level) {
                range_index.push_back(m);
            }
        }
        LOG_INFO("index size in range[`,`): `", min_level, max_level - 1, range_index.size());
        if (!range_index.size()) {
            LOG_DEBUG("return NULL");
            return nullptr;
        }
        return new Index(std::move(range_index));
    }

    virtual Index *rebuild_backing_index(Index *highlevel_idx, size_t max_level) {
        vector<SegmentMapping> mappings;
        const Index *indexes[2] = {highlevel_idx, const_cast<Index *>(m_backing_index)};
        merge_indexes(0, mappings, indexes, 2, 0, UINT64_MAX, false, max_level);
        return new Index(std::move(mappings));
    }

     virtual Index *make_read_only_index() const override{
        vector<SegmentMapping> mappings;
        auto ro_idx0 = new Index;
        ro_idx0->ownership = false;
        ro_idx0->assign(mapping.begin(), mapping.end());
        if (m_backing_index == nullptr) {
            return ro_idx0;
        }
        const Index *indexes[2] = {ro_idx0, const_cast<Index *>(m_backing_index)};
        merge_indexes(0, mappings, indexes, 2, 0, UINT64_MAX, false, 2);
        delete ro_idx0;
        return new Index(std::move(mappings));
    }

};

//======================== END OF ComboIndex =============================//

static bool verify_mapping_order(const SegmentMapping *pmappings, size_t n) {
    if (n < 2)
        return true;
    for (size_t i = 0; i < n - 1; ++i) {
        if (!(pmappings[i] < pmappings[i + 1])) {
            LOG_ERROR("incorrect segment mappings: disordered");
            return false;
        }
    }
    return true;
}
inline bool within(uint64_t x, uint64_t y, uint64_t begin, uint64_t end, uint32_t zeroed = 0) {
    if (zeroed) {
        return (begin <= x) && (x <= end);
    }
    return (begin <= x) && (x < end) && (begin < y) && (y <= end);
}

static bool verify_mapping_moffset(const SegmentMapping *pmappings, size_t n,
                                   uint64_t moffset_begin, uint64_t moffset_end) {
    for (auto &m : ptr_array(pmappings, n)) {
        if (!within(m.moffset, m.mend(), moffset_begin, moffset_end, m.zeroed)) {
            LOG_INFO("m.offset: `, m.moffset: `, m.length: ` m.zeroed: `", m.offset, m.moffset,
                     m.length, m.zeroed);
            LOG_ERROR("incorrect segment mappings [ ", m.moffset, " ", m.mend(), "] !within [ ",
                      moffset_begin, " ", moffset_end, " ]: mapped offset out of range");
            return false;
        }
    }
    return true;
}

IMemoryIndex0 *create_memory_index0(const SegmentMapping *pmappings, size_t n,
                                    uint64_t moffset_begin, uint64_t moffset_end) {
    auto ok = verify_mapping_moffset(pmappings, n, moffset_begin, moffset_end);
    return ok ? new Index0(pmappings, n) : nullptr;
}

IMemoryIndex *create_memory_index(const SegmentMapping *pmappings, size_t n, uint64_t moffset_begin,
                                  uint64_t moffset_end, bool ownership, uint64_t vsize) {
    auto ok1 = verify_mapping_order(pmappings, n);
    auto ok2 = verify_mapping_moffset(pmappings, n, moffset_begin, moffset_end);
    return (ok1 && ok2) ? new Index(pmappings, n, ownership, vsize) : nullptr;
}

IMemoryIndex *create_level_index(const SegmentMapping *pmappings, size_t n, uint64_t moffset_begin,
                                 uint64_t moffset_end, uint8_t copy_mode) {
    auto ok1 = verify_mapping_order(pmappings, n);
    auto ok2 = verify_mapping_moffset(pmappings, n, moffset_begin, moffset_end);
    return (ok1 && ok2) ? new LevelIndex(pmappings, n, copy_mode) : nullptr;
}

static void merge_indexes(uint8_t level, vector<SegmentMapping> &mapping, const Index **pindexes,
                          size_t n, uint64_t begin, uint64_t end, bool change_tag,
                          size_t max_level) {

    if (pindexes == nullptr)
        return;
    if (change_tag) {
        if (n == 0)
            return;
    } else {
        if (max_level == 0)
            return;
    }
    if (begin >= end)
        return;

    auto begin0 = begin;
    auto size0 = mapping.size();
    auto pi0 = pindexes[0];
    for (auto it = pi0->lower_bound(begin); it != pi0->end() && it->offset < end; ++it) {
        if (it->offset > begin) {
            if (change_tag)
                merge_indexes(level + 1, mapping, pindexes + 1, n - 1, begin, it->offset);
            else {
                int k = (n <= 1 ? 0 : 1);
                merge_indexes(level + 1, mapping, pindexes + k, 0, begin, it->offset, false,
                              max_level - 1);
            }
        }

        mapping.push_back(*it);
        if (change_tag) {
            mapping.back().tag = level;
        }
        begin = it->end();
    }
    if (begin < end) {
        if (change_tag)
            merge_indexes(level + 1, mapping, pindexes + 1, n - 1, begin, end);
        else {
            int k = (n <= 1 ? 0 : 1);
            merge_indexes(level + 1, mapping, pindexes + k, 0, begin, end, false, max_level - 1);
        }
    }
    if (mapping.size() > size0) {
        if (mapping[size0].offset < begin0)
            mapping[size0].forward_offset_to(begin0);
        if (mapping.back().end() > end)
            mapping.back().backward_end_to(end);
    }
}

IComboIndex *create_combo_index(IMemoryIndex0 *index0, const IMemoryIndex *index,
                                uint8_t ro_index_count, bool ownership) {
    if (!index0 || !index)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid argument(s)");

    auto i0 = (Index0 *)index0;
    auto i1 = (Index *)index;
    return new ComboIndex(i0, i1, ro_index_count, ownership);
}

size_t compress_raw_index(SegmentMapping *mapping, size_t n) {
    size_t i, j;
    if (n < 2)
        return n;
    verify_mapping_moffset(mapping, n, 0, INT64_MAX);

    for (j = 1, i = 0; j < n; ++j)
        if (mapping[i].end() == mapping[j].offset && mapping[i].mend() == mapping[j].moffset &&
            (!(mapping[i].zeroed ^ mapping[j].zeroed)) && mapping[i].tag == mapping[j].tag &&
            (uint64_t)(mapping[i].length + mapping[j].length) < SegmentMapping::MAX_LENGTH) {
            mapping[i].length += mapping[j].length;
        } else {
            mapping[++i] = mapping[j];
        }

    i++;
    LOG_INFO("index size compressed from ", n, " to ", i);
    return i;
}

size_t compress_raw_index_predict(const SegmentMapping *mapping, size_t n) {
    size_t i, j;
    if (n < 2)
        return n;
    auto m = mapping[0];
    for (j = 1, i = 0; j < n; ++j)
        if (m.end() == mapping[j].offset && m.tag == mapping[j].tag &&
            m.zeroed == mapping[j].zeroed &&
            (uint64_t)(m.length + mapping[j].length) < SegmentMapping::MAX_LENGTH) {
            m.length += mapping[j].length;
        } else {
            m = mapping[j];
            i++;
        }

    i++;
    LOG_INFO("index size predictively compressed from ", n, " to ", i);
    return i;
}

IMemoryIndex *merge_memory_indexes(const IMemoryIndex **pindexes, size_t n) {
    if (n > 255) {
        LOG_ERROR("too many indexes to merge, 255 at most!");
        return nullptr;
    }
    if (n == 0 || pindexes == nullptr)
        return nullptr;

    vector<SegmentMapping> mapping;
    auto pi = (const Index **)pindexes;
    mapping.reserve(pi[0]->size());
    merge_indexes(0, mapping, pi, n, 0, UINT64_MAX);
    return new Index(std::move(mapping), pindexes[0]->vsize());
}
} // namespace LSMT
