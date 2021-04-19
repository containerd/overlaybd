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
#include "file.h"
#include <string.h>
#include <stdarg.h>
#include <memory>
#include <atomic>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>
#include "index.h"
#include "../../alog.h"
#include "../../utility.h"
#include "../../photon/thread.h"

#define PARALLEL_LOAD_INDEX 32

using namespace std;
using namespace FileSystem;

typedef atomic<uint64_t> atomic_uint64_t;

namespace LSMT {
LogBuffer &operator<<(LogBuffer &log, Segment s) {
    return log.printf("Segment[", s.offset + 0, ',', s.length + 0, ']');
}
LogBuffer &operator<<(LogBuffer &log, const SegmentMapping &m) {
    return log.printf((Segment &)m, "--> Mapping[", m.moffset + 0, ',', m.zeroed + 0, ',',
                      m.tag + 0, ']');
}
struct HeaderTrailer {
    static const uint32_t SPACE = 4096;
    static const uint32_t TAG_SIZE = 256;
    static uint64_t MAGIC0() {
        static char magic0[] = "LSMT\0\1\2";
        return *(uint64_t *)magic0;
    }
    static constexpr UUID MAGIC1() {
        return {0xd2637e65, 0x4494, 0x4c08, 0xd2a2, {0xc8, 0xec, 0x4f, 0xcf, 0xae, 0x8a}};
    }
    // offset 0, 8
    uint64_t magic0 = MAGIC0();
    UUID magic1 = MAGIC1();
    bool verify_magic() const {
        return magic0 == HeaderTrailer::MAGIC0() && magic1 == HeaderTrailer::MAGIC1();
    }

    // offset 24, 28
    uint32_t size = sizeof(HeaderTrailer);
    uint32_t flags = 0;

    static const uint32_t FLAG_SHIFT_HEADER = 0; // 1:header         0:trailer
    static const uint32_t FLAG_SHIFT_TYPE = 1;   // 1:data file,     0:index file
    static const uint32_t FLAG_SHIFT_SEALED = 2; // 1:YES,           0:NO

    uint32_t get_flag_bit(uint32_t shift) const {
        return flags & (1 << shift);
    }
    void set_flag_bit(uint32_t shift) {
        flags |= (1 << shift);
    }
    void clr_flag_bit(uint32_t shift) {
        flags &= ~(1 << shift);
    }
    bool is_header() const {
        return get_flag_bit(FLAG_SHIFT_HEADER);
    }
    bool is_trailer() const {
        return !is_header();
    }
    bool is_data_file() const {
        return get_flag_bit(FLAG_SHIFT_TYPE);
    }
    bool is_index_file() const {
        return !is_data_file();
    }
    bool is_sealed() const {
        return get_flag_bit(FLAG_SHIFT_SEALED);
    }
    void set_header() {
        set_flag_bit(FLAG_SHIFT_HEADER);
    }
    void set_trailer() {
        clr_flag_bit(FLAG_SHIFT_HEADER);
    }
    void set_data_file() {
        set_flag_bit(FLAG_SHIFT_TYPE);
    }
    void set_index_file() {
        clr_flag_bit(FLAG_SHIFT_TYPE);
    }
    void set_sealed() {
        set_flag_bit(FLAG_SHIFT_SEALED);
    }
    void clr_sealed() {
        clr_flag_bit(FLAG_SHIFT_SEALED);
    }

    void set_uuid(const UUID uuid) {
        this->uuid = uuid;
    }

    int set_tag(char *buf, size_t n) {
        if (n > TAG_SIZE) {
            // auto tag_size = TAG_SIZE;  // work around for compiler err (gcc 4.9.2)..
            LOG_ERROR_RETURN(ENOBUFS, -1, "user tag too long. (need less than `)",
                             static_cast<uint32_t>(TAG_SIZE));
        }
        if (n == 0) {
            memset(user_tag, 0, sizeof(user_tag));
            return 0;
        }
        memcpy(user_tag, buf, n);
        return 0;
    }

    // offset 32, 40, 48
    uint64_t index_offset; // in bytes
    uint64_t index_size;   // # of SegmentMappings
    uint64_t virtual_size; // in bytes

    UUID::String uuid;        // 37 bytes.
    UUID::String parent_uuid; // 37 bytes.
    uint8_t from;             // DEPRECATED
    uint8_t to;               // DEPRECATED

    static const uint8_t LSMT_V1 = 1;     // v1 (UUID check)
    static const uint8_t LSMT_SUB_V1 = 1; // .1 deprecated level range.

    uint8_t version = LSMT_V1;
    uint8_t sub_version = LSMT_SUB_V1;

    char user_tag[TAG_SIZE]{}; // 256B commit message.

} __attribute__((packed));

class LSMTReadOnlyFile;
static LSMTReadOnlyFile *open_file_ro(IFile *file, bool ownership, bool reserve_tag);

static const uint32_t ALIGNMENT = 512; // same as trim block size.
static const uint32_t ALIGNMENT4K = 4096;

static const int ABORT_FLAG_DETECTED = -2;

class LSMTReadOnlyFile : public IFileRW {
public:
    size_t MAX_IO_SIZE = 4 * 1024 * 1024;
    uint64_t m_vsize = 0;
    vector<IFile *> m_files;
    vector<UUID> m_uuid;
    IMemoryIndex *m_index = nullptr;
    bool m_file_ownership = false;
    uint64_t m_data_offset = HeaderTrailer::SPACE / ALIGNMENT;
    uint32_t lsmt_io_cnt = 0;
    uint64_t lsmt_io_size = 0;

    virtual ~LSMTReadOnlyFile() {
        LOG_DEBUG("pread times: `, size: `M", lsmt_io_cnt, lsmt_io_size >> 20);
        close();
        if (m_file_ownership) {
            LOG_DEBUG("m_file_ownership:`, m_files.size:`", m_file_ownership, m_files.size());
            for (auto &x : m_files)
                safe_delete(x);
        }
    }

    virtual int set_max_io_size(size_t _size) override {

        if (_size == 0 || (_size & (ALIGNMENT4K - 1)) != 0) {
            LOG_ERROR_RETURN(0, -1, "_size( ` ) is not aligned with 4K.", _size);
        }
        LOG_INFO("`", _size);
        this->MAX_IO_SIZE = _size;
        return 0;
    }

    virtual size_t get_max_io_size() override {
        return this->MAX_IO_SIZE;
    }

    virtual IMemoryIndex0 *index() const override {
        return (IMemoryIndex0 *)m_index;
    }

    virtual int close() override {
        safe_delete(m_index);
        if (m_file_ownership) {
            for (auto &x : m_files)
                if (x)
                    x->close();
        }
        return 0;
    }

    virtual int get_uuid(UUID &out, size_t layer_id) const override {
        if (layer_id >= m_uuid.size()) {
            LOG_ERROR_RETURN(0, -1, "layer_id out of range.");
        }
        out = m_uuid[layer_id];
        LOG_DEBUG(out);
        return 0;
    }

    template <typename T1, typename T2, typename T3>
    inline void forward(void *&buffer, T1 &offset, T2 &count, T3 step) {
        (char *&)buffer += step * ALIGNMENT;
        offset += step;
        count -= step;
    }
    template <typename T>
    bool is_aligned(T x) {
        static_assert(std::is_integral<T>::value, "T must be integral types");
        return (x & (ALIGNMENT - 1)) == 0;
    }
#define CHECK_ALIGNMENT(size, offset)                                                              \
    if (!is_aligned((size) | (offset)))                                                            \
        LOG_ERROR_RETURN(EFAULT, -1, "arguments must be aligned!");

    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {
        CHECK_ALIGNMENT(count, offset);
        auto nbytes = count;
        while (count > MAX_IO_SIZE) {
            auto ret = pread(buf, MAX_IO_SIZE, offset);
            if (ret < (ssize_t)MAX_IO_SIZE)
                return -1;
            (char *&)buf += MAX_IO_SIZE;
            count -= MAX_IO_SIZE;
            offset += MAX_IO_SIZE;
        }
        count /= ALIGNMENT;
        offset /= ALIGNMENT;
        Segment s{(uint64_t)offset, (uint32_t)count};
        auto ret = foreach_segments(
            m_index, s,
            [&](const Segment &m) __attribute__((always_inline)) {
                auto step = m.length * ALIGNMENT;
                memset(buf, 0, step);
                (char *&)buf += step;
                return 0;
            },
            [&](const SegmentMapping &m) __attribute__((always_inline)) {
                if (m.tag >= m_files.size()) {
                    LOG_DEBUG(" ` >= `", m.tag, m_files.size());
                }
                assert(m.tag < m_files.size());
                ssize_t size = m.length * ALIGNMENT;
                LOG_DEBUG("offse: `, length: `", m.moffset, size);
                ssize_t ret = m_files[m.tag]->pread(buf, size, m.moffset * ALIGNMENT);
                if (ret < size) {
                    LOG_ERROR_RETURN(0, (int)ret,
                                     "failed to read from `-th file ( ` pread return: ` < size: `)",
                                     m.tag, m_files[m.tag], ret, size);
                }
                lsmt_io_size += ret;
                lsmt_io_cnt++;
                (char *&)buf += size;
                return 0;
            });
        return (ret >= 0) ? nbytes : ret;
    }

    IFile *front_file() {
        for (auto x : m_files)
            if (x)
                return x;
        return nullptr;
    }
    virtual int fstat(struct stat *buf) override {
        auto file = front_file();
        if (!file)
            LOG_ERROR_RETURN(ENOSYS, -1, "no underlying files found!");

        auto ret = file->fstat(buf);
        if (ret == 0) {
            buf->st_blksize = ALIGNMENT;
            buf->st_size = m_vsize;
            buf->st_blocks = m_index->block_count();
        }
        return ret;
    }
    virtual ::FileSystem::IFileSystem *filesystem() override {
        auto file = front_file();
        if (!file)
            LOG_ERROR_RETURN(ENOSYS, nullptr, "no underlying files found!");
        return file->filesystem();
    }
    UNIMPLEMENTED(int close_seal(IFileRO **reopen_as = nullptr) override);
    UNIMPLEMENTED(int commit(const CommitArgs &args) const override);

    virtual DataStat data_stat() const override {
        uint64_t size = 0;
        auto idx_arr = ptr_array(m_index->buffer(), m_index->size());
        for (auto x : idx_arr)
            size += x.length * (!x.zeroed);
        size *= ALIGNMENT;
        DataStat ret;
        ret.total_data_size = size;
        ret.valid_data_size = size;
        return ret;
    }
};

static int write_header_trailer(IFile *file, bool is_header, bool is_sealed, bool is_data_file,
                                uint64_t index_offset, uint64_t index_size, const LayerInfo &args) {
    ALIGNED_MEM(buf, HeaderTrailer::SPACE, ALIGNMENT4K);
    memset(buf, 0, HeaderTrailer::SPACE);
    auto pht = new (buf) HeaderTrailer;

    if (is_header)
        pht->set_header();
    else
        pht->set_trailer();
    if (is_sealed)
        pht->set_sealed();
    else
        pht->clr_sealed();
    if (is_data_file)
        pht->set_data_file();
    else
        pht->set_index_file();

    pht->index_offset = index_offset;
    pht->index_size = index_size;
    pht->virtual_size = args.virtual_size;
    if (pht->set_tag(args.user_tag, args.len) != 0)
        return -1;
    if (is_header) {
        LOG_DEBUG("set header UUID");
    } else {
        LOG_DEBUG("set trailer UUID");
    }
    pht->set_uuid(args.uuid);
    pht->parent_uuid = args.parent_uuid;
    if (args.parent_uuid.is_null()) {
        LOG_WARN("parent_uuid is null.");
    }
    return (int)file->write(buf, HeaderTrailer::SPACE);
}

struct CompactOptions {

    IFile **src_files;
    size_t n;
    SegmentMapping *raw_index;
    size_t index_size;
    size_t virtual_size;
    const CommitArgs *commit_args = nullptr;
    uint64_t io_usleep_time = 0;
    char *TRIM_BLOCK = nullptr;
    size_t trim_blk_size = 0;
};

static int push_segment(char *buf, char *data, int &data_length, int &prev_end, int zero_detect,
                        SegmentMapping &s, vector<SegmentMapping> &index) {
    auto start_moffset = s.moffset;
    if (zero_detect == 1)
        s.discard();
    else {
        memcpy(data + data_length, buf + prev_end * ALIGNMENT, s.length * ALIGNMENT);
        data_length += s.length * ALIGNMENT;
    }
    prev_end += s.length;
    LOG_DEBUG("push ", s, ' ', VALUE(data_length));
    index.push_back(s);
    start_moffset = s.mend();
    s.zeroed = 0;
    s.offset = s.end();
    s.length = 0;
    s.moffset = start_moffset;
    return 0;
}

static int is_zero_block(char *buf, size_t n) {
    return 1;
    // TODO: fixme: result data error
    if (n & (sizeof(uint64_t) - 1)) {
        LOG_ERROR("buf size invalid.");
        return -1;
    }
    for (uint64_t j = 0; j < n; j += sizeof(uint64_t)) {
        if ((uint64_t)(*(buf + j)) == (uint64_t)0)
            continue;
        return 1;
    }
    return 0;
}

static ssize_t pcopy(const CompactOptions &opt, const SegmentMapping &m, uint64_t moffset,
                     vector<SegmentMapping> &index) {
    auto offset = m.moffset * ALIGNMENT;
    auto count = m.length * ALIGNMENT;
    auto bytes = 0;
    const size_t BUFFER_SIZE = 32 * 1024;
    ALIGNED_MEM4K(buf, BUFFER_SIZE);
    ALIGNED_MEM4K(data, BUFFER_SIZE);
    LOG_DEBUG("check segment: [ offset: `, len: `, moffset: `]", m.offset, m.length, m.moffset);
    SegmentMapping s{m.offset, 0, moffset, m.tag};
    while (count > 0) {
        ssize_t step = min((size_t)count, BUFFER_SIZE /* BUFFER_SIZE 32K */);
        LOG_DEBUG("read from src_file, offset: `, step: `", offset, step);
        ssize_t ret = opt.src_files[m.tag]->pread(buf, step, offset);
        if (ret < (ssize_t)step)
            LOG_ERRNO_RETURN(0, -1, "failed to read from file");
        auto zero_detected = -1;
        auto data_length = 0;
        auto prev_end = 0;
        for (auto i = 0; i < step; i += (ssize_t)ALIGNMENT) {
            if (is_zero_block(buf + i, ALIGNMENT) == 0) {
                if (zero_detected == 0 && s.length) {
                    push_segment(buf, data, data_length, prev_end, zero_detected, s, index);
                }
                s.length++;
                zero_detected = 1;
                continue;
            }
            if (zero_detected == 1) {
                push_segment(buf, data, data_length, prev_end, zero_detected, s, index);
            }
            zero_detected = 0;
            s.length++;
        }
        if (s.length) {
            push_segment(buf, data, data_length, prev_end, zero_detected, s, index);
        }
        /* write non-zeroed data */
        LOG_DEBUG("write valid data(size: `)", data_length);
        if (data_length) {
            ret = opt.commit_args->as->write(data, data_length);
            if (ret < (ssize_t)data_length)
                LOG_ERROR_RETURN(0, -1, "failed to write to file");
        }
        bytes += data_length;
        offset += step;
        count -= step;
    }
    return bytes / ALIGNMENT;
}

static int load_layer_info(IFile **src_files, size_t n, LayerInfo &layer, bool oper_seal = false) {
    ALIGNED_MEM(buf_top, HeaderTrailer::SPACE, ALIGNMENT4K);
    auto ret = src_files[0]->pread(buf_top, HeaderTrailer::SPACE, 0);
    if (ret != HeaderTrailer::SPACE) {
        LOG_ERROR_RETURN(0, -1, "read layer info failed.");
    }
    HeaderTrailer *pht = (HeaderTrailer *)buf_top;
    layer.virtual_size = pht->virtual_size;
    if (n != 1) {
        ALIGNED_MEM(buf_bottom, HeaderTrailer::SPACE, ALIGNMENT4K);
        //
        auto ret = src_files[n - 1]->pread(buf_bottom, HeaderTrailer::SPACE, 0);
        if (ret != HeaderTrailer::SPACE) {
            LOG_ERROR_RETURN(0, -1, "read bottom info failed.");
        }
        pht = (HeaderTrailer *)buf_bottom;
        if (layer.parent_uuid.parse(pht->parent_uuid) == 0) {
            LOG_INFO("get parent UUID: `", pht->parent_uuid);
        } else {
            LOG_WARN("bottom layer's uuid get null.");
        }
    } else {
        LOG_DEBUG(pht->parent_uuid);
        if (layer.parent_uuid.parse(pht->parent_uuid) == 0) {
            LOG_INFO("get parent UUID: `", pht->parent_uuid);
        } else {
            LOG_WARN("top layer's parent_uuid get null.");
        }
    }
    if (oper_seal) {
        LOG_INFO("close_seal detected. Sealed trailer's UUID should same with its headers'");
        if (layer.uuid.parse(pht->uuid) != 0) {
            LOG_WARN("top layer's uuid get null.");
        }
    }
    return 0;
}

static int compact(const CompactOptions &opt, atomic_uint64_t &compacted_idx_size) {
    auto src_files = opt.src_files;
    auto commit_args = opt.commit_args;
    auto dest_file = commit_args->as;

    LayerInfo layer;
    if (load_layer_info(src_files, opt.n, layer) != 0)
        return -1;
    layer.user_tag = commit_args->user_tag;
    if (UUID::String::is_valid((commit_args->parent_uuid).c_str())) {
        layer.parent_uuid.parse(commit_args->parent_uuid);
    }
    layer.len = commit_args->get_tag_len();
    ssize_t ret = write_header_trailer(dest_file, true, true, true, 0, 0, layer);
    if (ret < 0) {
        LOG_ERRNO_RETURN(0, -1, "failed to write header.");
    }
    auto marray = ptr_array(opt.raw_index, opt.index_size);
    uint64_t moffset = HeaderTrailer::SPACE;
    vector<SegmentMapping> compact_index;
    moffset /= ALIGNMENT;
    for (auto &m : marray) {
        compacted_idx_size.fetch_add(1);
        if (m.zeroed) {
            m.moffset = moffset;
            compact_index.push_back(m);
            // there is no need do pcopy if current block is zero-marked.
            continue;
        }
        auto ret = pcopy(opt, m, moffset, compact_index);
        if (ret < 0)
            return (int)ret;
        moffset += ret;
    }
    uint64_t index_offset = moffset * ALIGNMENT;
    auto index_size = compress_raw_index(&compact_index[0], compact_index.size());
    LOG_DEBUG("write index to dest_file `, size: `*`", dest_file, index_size,
              sizeof(SegmentMapping));

    ALIGNED_MEM4K(raw, 4096);
    int N = ALIGNMENT4K / sizeof(SegmentMapping);
    ssize_t p = 0;
    size_t padding = N - index_size % N;
    if ((ssize_t)padding < N) {
        compact_index.resize(index_size + padding);
        for (size_t i = index_size; i < index_size + padding; i++)
            compact_index[i] = SegmentMapping::invalid_mapping();
        LOG_DEBUG("index_count: `, (include padding: `), `", compact_index.size(), padding,
                  sizeof(SegmentMapping));
        assert(compact_index.size() % N == 0);
        index_size += padding;
    } else {
        compact_index.resize(index_size);
    }
    size_t writen = 0;
    while (p < (ssize_t)compact_index.size()) {
        memcpy(raw, &compact_index[p], ALIGNMENT4K);
        ret = dest_file->write(raw, ALIGNMENT4K);
        assert(ret == ALIGNMENT4K);
        writen += ret;
        p += N;
    }
    assert(writen == index_size * sizeof(SegmentMapping));
    auto trailer_offset = dest_file->lseek(0, 2);
    LOG_DEBUG("trailer offset: `", trailer_offset);
    ret = write_header_trailer(dest_file, false, true, true, index_offset, index_size, layer);
    if (ret < 0)
        LOG_ERROR_RETURN(0, -1, "failed to write trailer");
    return 0;
}

class LSMTFile : public LSMTReadOnlyFile {
public:
    typedef photon::mutex Mutex;
    typedef photon::scoped_lock Lock;

    atomic_uint64_t m_compacted_idx_size; // count of compacted raw-index

    bool m_init_concurrency = false;
    uint64_t m_data_offset = HeaderTrailer::SPACE / ALIGNMENT;

    Mutex m_rw_mtx;
    IFile *m_findex = nullptr;

    vector<SegmentMapping> m_stacked_mappings;
    // used as a buffer for batch write (aka "group commit")
    uint32_t nmapping = 0;
    // # of elements in the mapping buffer

    LSMTFile() {
        m_compacted_idx_size.store(0);
    }

    ~LSMTFile() {
        LOG_DEBUG(" ~LSMTFile()");
        close();
    }

    virtual int vioctl(int request, va_list args) override {
        if (request != Index_Group_Commit)
            LOG_ERROR_RETURN(EINVAL, -1, "invaid request code");

        auto buffer_size = va_arg(args, size_t);
        buffer_size /= sizeof(SegmentMapping);
        if (buffer_size < nmapping) {
            Lock lock(m_rw_mtx);
            do_group_commit_mappings();
        }

        m_stacked_mappings.resize(buffer_size);
        return 0;
    }

    virtual int init_concurrency() {
        if (m_init_concurrency)
            return 0;
        LOG_DEBUG("Initialize concurrency variables (mutex & cond).");
        m_init_concurrency = true;
        return 0;
    }

    virtual int close() override {
        LOG_DEBUG("ownership:`, m_finde:`", m_file_ownership, m_findex);
        {
            Lock lock(m_rw_mtx);
            do_group_commit_mappings();
        }
        if (m_file_ownership)
            safe_delete(m_findex);
        return LSMTReadOnlyFile::close();
    }
    // returns appended offset when success, 0 therwise
    static off_t append(IFile *file, const void *buf, size_t count) {
        off_t pos = file->lseek(0, SEEK_END);
        ssize_t ret = file->write(buf, count);
        if (ret < (ssize_t)count) {
            LOG_ERRNO_RETURN(0, 0, "write failed, file:`, ret:`, pos:`, count:`", file, ret, pos,
                             count);
        }
        return pos;
    }

    int do_group_commit_mappings() {
        if (nmapping > 0) {
            while (nmapping < m_stacked_mappings.size()) {
                m_stacked_mappings[nmapping++] = SegmentMapping::invalid_mapping();
            }
            auto index_size = nmapping * sizeof(m_stacked_mappings[0]);
            ALIGNED_MEM4K(raw, index_size);
            memcpy(raw, &m_stacked_mappings[0], index_size);
            auto ret = append(m_findex, raw, index_size);
            if (ret == 0)
                return -1;
            nmapping = 0;
        }
        return 0;
    }

    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {
        return LSMTReadOnlyFile::pread(buf, count, offset);
    }

    virtual void append_index(const SegmentMapping &m) {

        if (m_findex) {
            if (m_stacked_mappings.empty()) {
                Lock lock(m_rw_mtx);
                append(m_findex, &m, sizeof(m));
            } else {
                m_stacked_mappings[nmapping++] = m;
                if (nmapping == m_stacked_mappings.size() /* || TODO: timeout  */) {
                    Lock lock(m_rw_mtx);
                    do_group_commit_mappings();
                }
            }
        }
    }

    virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override {
        return VirtualFile::pwritev(iov, iovcnt, offset);
    }

    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        LOG_DEBUG("{offset:`,length:`}", offset, count);
        CHECK_ALIGNMENT(count, offset);

        auto bytes = count;
        while (count > MAX_IO_SIZE) {
            auto ret = pwrite(buf, MAX_IO_SIZE, offset);
            if (ret < (ssize_t)MAX_IO_SIZE)
                return -1;
            (char *&)buf += MAX_IO_SIZE;
            count -= MAX_IO_SIZE;
            offset += MAX_IO_SIZE;
        }
        // wait unlock
        off_t moffset = -1;
        {
            Lock lock(m_rw_mtx);
            moffset = append(m_files[0], buf, count);
            if (moffset == 0)
                return -1;
        }
        m_vsize = max(m_vsize, count + offset);
        if (m_vsize < count + offset) {
            LOG_INFO("resize m_visze: `->`", m_vsize, count + offset);
        }
        SegmentMapping m{(uint64_t)offset / (uint64_t)ALIGNMENT,
                         (uint32_t)count / (uint32_t)ALIGNMENT,
                         (uint64_t)moffset / (uint64_t)ALIGNMENT};
        assert(m.length > (uint32_t)0);
        m_data_offset = m.mend();
        static_cast<IMemoryIndex0 *>(m_index)->insert(m);
        append_index(m);
        return bytes;
    }

#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01 /* default is extend size */
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE 0x02 /* de-allocates range */
#endif
    virtual int fallocate(int mode, off_t offset, off_t len) override {
        auto max_length_bytes = Segment::MAX_LENGTH * ALIGNMENT;

        while (len > max_length_bytes) {
            auto ret = this->fallocate(mode, offset, max_length_bytes);
            if (ret != 0)
                return -1;
            offset += max_length_bytes;
            len -= max_length_bytes;
        }
        if (((mode & FALLOC_FL_PUNCH_HOLE) == 0) || ((mode & FALLOC_FL_KEEP_SIZE) == 0)) {
            LOG_ERRNO_RETURN(ENOSYS, -1, "only support FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE");
        }
        return this->discard(offset, len);
    }

    virtual int discard(off_t offset, off_t len) {
        CHECK_ALIGNMENT(len, offset);
        off_t pos = m_files[0]->lseek(0, SEEK_END);
        SegmentMapping m{(uint64_t)offset / (uint64_t)ALIGNMENT,
                         (uint32_t)len / (uint32_t)ALIGNMENT, (uint64_t)(pos / ALIGNMENT)};
        m.discard();
        static_cast<IMemoryIndex0 *>(m_index)->insert(m);
        append_index(m);
        return 0;
    }

    virtual int commit(const CommitArgs &args) const override {
        if (m_files.size() > 1) {
            LOG_ERROR_RETURN(ENOTSUP, -1, "not supported: commit stacked files");
        }
        auto m_index0 = (IMemoryIndex0 *)m_index;
        unique_ptr<SegmentMapping[]> mapping(m_index0->dump());

        CompactOptions opts;
        opts.src_files = (IFile **)&m_files[0];
        opts.n = m_files.size();
        opts.raw_index = mapping.get();
        opts.index_size = m_index->size();
        opts.virtual_size = m_vsize;
        opts.commit_args = &args;
        atomic_uint64_t _no_use_var(0);
        return compact(opts, _no_use_var);
    }

    virtual int close_seal(IFileRO **reopen_as = nullptr) override {
        auto m_index0 = (IMemoryIndex0 *)m_index;
        unique_ptr<SegmentMapping[]> mapping(m_index0->dump(ALIGNMENT));
        uint64_t index_offset = m_files[0]->lseek(0, SEEK_END);
        ssize_t index_bytes = m_index0->size() * sizeof(SegmentMapping);
        index_bytes = (index_bytes + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT;
        auto ret = m_files[0]->write(mapping.get(), index_bytes);
        if (ret < index_bytes)
            LOG_ERRNO_RETURN(0, -1, "failed to write index.");

        LayerInfo layer;
        if (load_layer_info(&m_files[0], 1, layer, true) != 0)
            return -1;
        ret = write_header_trailer(m_files[0], false, true, true, index_offset, m_index0->size(),
                                   layer);
        if (ret < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to write trailer.");
        if (reopen_as) {
            auto new_index =
                create_memory_index(mapping.release(), m_index0->size(),
                                    HeaderTrailer::SPACE / ALIGNMENT, index_offset / ALIGNMENT);
            if (new_index == nullptr) {
                LOG_ERROR("create memory index of reopen file failed.");
                return close();
            }
            new_index->increase_tag();
            auto p = new LSMTReadOnlyFile;
            p->m_index = new_index;
            p->m_files = {nullptr, m_files.back()};
            p->m_vsize = m_vsize;
            p->m_file_ownership = m_file_ownership;
            m_file_ownership = false;
            *reopen_as = p;
        }
        return close();
    }

    virtual int fsync() override {
        {
            Lock lock(m_rw_mtx);
            auto commit_ret = do_group_commit_mappings();
            if (commit_ret != 0) {
                return commit_ret;
            }
        }
        m_files[0]->fsync();
        if (m_findex)
            m_findex->fsync();
        return 0;
    }
    virtual int fdatasync() override {
        return fsync();
    }
    virtual int sync_file_range(off_t offset, off_t nbytes, unsigned int flags) override {
        return fsync();
    }
    virtual int fchmod(mode_t mode) override {
        return 0;
    }
    virtual int fchown(uid_t owner, gid_t group) override {
        return 0;
    }

    virtual DataStat data_stat() const override {
        struct stat buf;
        auto ret = m_files[0]->fstat(&buf);
        if (ret != 0) {
            LOG_ERRNO_RETURN(0, DataStat(), "failed to fstat()");
        }

        DataStat data_stat;
        data_stat.total_data_size = (buf.st_size - HeaderTrailer::SPACE);
        data_stat.valid_data_size = index()->block_count() * ALIGNMENT;
        LOG_DEBUG("data_size: ` ( valid: ` )", data_stat.total_data_size,
                  data_stat.valid_data_size);
        return data_stat;
    }
};

HeaderTrailer *verify_ht(IFile *file, char *buf) {
    if (file == nullptr) {
        LOG_ERRNO_RETURN(0, nullptr, "invalid file ptr (null).");
    }
    auto ret = file->pread(buf, HeaderTrailer::SPACE, 0);
    if (ret < (ssize_t)HeaderTrailer::SPACE)
        LOG_ERRNO_RETURN(0, nullptr, "failed to read file header.");

    auto pht = (HeaderTrailer *)buf;
    if (!pht->verify_magic() || !pht->is_header())
        LOG_ERROR_RETURN(0, nullptr, "header magic/type don't match");
    return pht;
}

static SegmentMapping *do_load_index(IFile *file, HeaderTrailer *pheader_trailer, bool trailer) {
    ALIGNED_MEM(buf, HeaderTrailer::SPACE, ALIGNMENT4K);
    auto pht = verify_ht(file, buf);
    if (pht == nullptr)
        return nullptr;
    struct stat stat;
    auto ret = file->fstat(&stat);
    if (ret < 0)
        LOG_ERRNO_RETURN(0, nullptr, "failed to stat file.");

    uint64_t index_bytes;
    if (trailer) {
        if (!pht->is_data_file())
            LOG_ERROR_RETURN(0, nullptr, "uncognized file type");

        auto trailer_offset = stat.st_size - HeaderTrailer::SPACE;
        ret = file->pread(buf, HeaderTrailer::SPACE, trailer_offset);
        if (ret < (ssize_t)HeaderTrailer::SPACE)
            LOG_ERRNO_RETURN(0, nullptr, "failed to read file trailer.");

        if (!pht->verify_magic() || !pht->is_trailer() || !pht->is_data_file() || !pht->is_sealed())
            LOG_ERROR_RETURN(0, nullptr,
                             "trailer magic, trailer type, "
                             "file type or sealedness doesn't match");
        LOG_DEBUG("index_size: `, trailer offset: `", pht->index_size + 0, trailer_offset);
        index_bytes = pht->index_size * sizeof(SegmentMapping);

        if (index_bytes > trailer_offset - pht->index_offset)
            LOG_ERROR_RETURN(0, nullptr, "invalid index bytes or size");

    } else {
        if (!pht->is_index_file() || pht->is_sealed())
            LOG_ERROR_RETURN(0, nullptr, "file type or sealedness wrong");
        if (pht->index_offset != HeaderTrailer::SPACE)
            LOG_ERROR_RETURN(0, nullptr, "index offset wrong");
        index_bytes = stat.st_size - HeaderTrailer::SPACE;
        pht->index_size = index_bytes / sizeof(SegmentMapping);
    }

    SegmentMapping *ibuf = nullptr;
    posix_memalign((void **)&ibuf, ALIGNMENT4K, pht->index_size * sizeof(*ibuf));
    ret = file->pread(ibuf, index_bytes, pht->index_offset);
    if (ret < (ssize_t)index_bytes) {
        free(ibuf);
        LOG_ERROR_RETURN(0, nullptr, "failed to read index.");
    }

    size_t index_size = 0;
    for (size_t i = 0; i < pht->index_size; ++i)
        if (ibuf[i].offset != SegmentMapping::INVALID_OFFSET) {
            ibuf[index_size] = ibuf[i];
            ibuf[index_size].tag = 0;
            index_size++;
        }

    pht->index_size = index_size;
    if (pheader_trailer)
        *pheader_trailer = *pht;

    auto p = new SegmentMapping[index_size];
    memcpy(p, ibuf, index_size * sizeof(*p));
    free(ibuf);
    return p;
}

static LSMTReadOnlyFile *open_file_ro(IFile *file, bool ownership, bool reserve_tag) {
    if (!file) {
        LOG_ERROR("invalid file ptr. file: `", file);
        return nullptr;
    }

    HeaderTrailer ht;
    auto p = do_load_index(file, &ht, true);
    if (!p)
        LOG_ERROR_RETURN(EIO, nullptr, "failed to load index from file.");
    auto pi = create_memory_index(p, ht.index_size, HeaderTrailer::SPACE / ALIGNMENT,
                                  ht.index_offset / ALIGNMENT);
    if (!pi) {
        delete[] p;
        LOG_ERROR_RETURN(0, nullptr, "failed to create memory index!");
    }
    if (reserve_tag) {
        pi->increase_tag();
    }
    auto rst = new LSMTReadOnlyFile;
    rst->m_index = pi;
    rst->m_files = {nullptr, file};
    rst->m_uuid.resize(2);
    rst->m_uuid[1].parse(ht.uuid);
    rst->m_vsize = ht.virtual_size;
    rst->m_file_ownership = ownership;
    LOG_INFO("Layer Info: { UUID: `, Parent_UUID: `, Virtual size: `, Version: `.` }", ht.uuid,
             ht.parent_uuid, rst->m_vsize, ht.version, ht.sub_version);
    return rst;
}

IFileRO *open_file_ro(IFile *file, bool ownership) {
    return open_file_ro(file, ownership, true);
}

IFileRW *open_file_rw(IFile *fdata, IFile *findex, bool ownership) {
    ALIGNED_MEM(buf, HeaderTrailer::SPACE, ALIGNMENT4K);
    auto pht = verify_ht(fdata, buf);
    if ((pht == nullptr) || !findex) {
        LOG_ERRNO_RETURN(0, nullptr, "invalid file ptr, fdata: ` findex: `", fdata, findex);
    }
    struct stat stat;
    int ret = fdata->fstat(&stat);
    if (ret < 0) {
        LOG_ERRNO_RETURN(0, nullptr, "failed to stat data file.");
    }
    IMemoryIndex0 *pi = nullptr;

    HeaderTrailer ht;
    auto p = do_load_index(findex, &ht, false);
    if (!p) {
        LOG_ERROR_RETURN(EIO, nullptr, "failed to load index from file.");
    }
    DEFER(delete[] p);
    pi = create_memory_index0(p, ht.index_size, HeaderTrailer::SPACE / ALIGNMENT,
                              stat.st_size / ALIGNMENT);
    if (!pi) {
        LOG_ERROR_RETURN(0, nullptr, "failed to create memory index!");
    }
    pht = &ht;

    LSMTFile *rst = new LSMTFile;
    rst->m_index = pi;
    rst->m_findex = findex;
    rst->m_files.push_back(fdata);
    rst->m_vsize = pht->virtual_size;
    rst->m_file_ownership = ownership;
    UUID raw;
    raw.parse(pht->uuid);
    rst->m_uuid.push_back(raw);
    LOG_INFO("Layer Info: { UUID:` , Parent_UUID: `, Virtual size: `, Version: `.` }", pht->uuid,
             pht->parent_uuid, rst->m_vsize, pht->version, pht->sub_version);
    return rst;
}

IFileRW *create_file_rw(const LayerInfo &args, bool ownership) {
    auto fdata = args.fdata;
    auto findex = args.findex;
    if (!fdata || !findex) {
        LOG_ERROR_RETURN(0, nullptr, "invalid file ptr, fdata: `, findex: `", fdata, findex);
    }
    LSMTFile *rst = new LSMTFile;
    rst->m_index = create_memory_index0((const SegmentMapping *)nullptr, 0, 0, 0);
    rst->m_findex = findex;
    rst->m_files.push_back(fdata);
    LOG_DEBUG("unparse uuid");
    UUID raw;
    raw.parse(args.uuid);
    rst->m_uuid.push_back(raw);
    LOG_DEBUG("RWFile uuid: `", rst->m_uuid[0]);
    rst->m_vsize = args.virtual_size;
    rst->m_file_ownership = ownership;
    write_header_trailer(fdata, true, false, true, 0, 0, args);
    write_header_trailer(findex, true, false, false, HeaderTrailer::SPACE, 0, args);
    HeaderTrailer tmp;
    LOG_INFO("Layer Info: { UUID:`, Parent_UUID: `, Virtual size: `, Version: `.` }", raw,
             args.parent_uuid, rst->m_vsize, tmp.version, tmp.sub_version);
    return rst;
}

struct parallel_load_task {

    IFile **files;
    vector<unique_ptr<IMemoryIndex>> indexes;
    int eno = 0;

    struct Job {
        parallel_load_task *tm;
        HeaderTrailer ht;
        size_t i;
        uint8_t eno = 0;
        IFile *get_file() {
            return tm->files[i];
        }
        void set_index(IMemoryIndex *idx) {
            tm->indexes[i].reset(idx);
        }
        void set_error(int eno) {
            tm->eno = this->eno = eno;
        }
    };

    vector<Job> jobs;
    size_t i = 0, nlayers;
    Job *get_job() {
        LOG_DEBUG("create job, layer_id: `", i);
        if (i < nlayers) {
            auto j = &jobs[i];
            j->tm = this;
            j->i = i++;
            return j;
        }
        return nullptr;
    }

    Job *get_result(size_t i) {
        return &jobs[i];
    }

    parallel_load_task(IFile **files, size_t nlayers) {
        this->nlayers = nlayers;
        this->files = files;
        indexes.resize(nlayers);
        jobs.resize(nlayers);
    }
};

void *do_parallel_load_index(void *param) {
    parallel_load_task *tm = (parallel_load_task *)param;
    while (true) {
        auto job = tm->get_job();
        if (job == nullptr || tm->eno != 0) {
            // error occured from another threads.
            return nullptr;
        }
        auto p = do_load_index(job->get_file(), &job->ht, true);
        if (!p) {
            job->set_error(EIO);
            LOG_ERROR_RETURN(0, nullptr, "failed to load index from `-th file", job->i);
        }
        auto pi = create_memory_index(p, job->ht.index_size, HeaderTrailer::SPACE / ALIGNMENT,
                                      job->ht.index_offset / ALIGNMENT);
        if (!pi) {
            delete[] p;
            job->set_error(EIO);
            LOG_ERROR_RETURN(0, nullptr, "failed to create memory index!");
        }
        job->set_index(pi);
    }
    return NULL;
}

static IMemoryIndex *load_merge_index(vector<IFile *> &files, vector<UUID> &uuid,
                                      HeaderTrailer &ht) {
    photon::join_handle *ths[PARALLEL_LOAD_INDEX];
    auto n = min(PARALLEL_LOAD_INDEX, (int)files.size());
    LOG_DEBUG("create ` photon threads to merge index", n);
    parallel_load_task tm((IFile **)&(files[0]), files.size());
    for (auto i = 0; i < n; ++i) {
        ths[i] = photon::thread_enable_join(photon::thread_create(&do_parallel_load_index, &tm));
    }
    for (int i = 0; i < n; ++i) {
        photon::thread_join(ths[i]);
    }
    if (tm.eno != 0) {
        LOG_ERROR_RETURN(tm.eno, nullptr, "load index failed.");
    }
    for (size_t i = 0; i < files.size(); i++) {
        auto job = tm.get_result(i);
        uuid[i].parse(job->ht.uuid);
    }
    assert(tm.jobs.back().i == files.size() - 1);
    ht = tm.jobs.back().ht;
    std::reverse(files.begin(), files.end());
    std::reverse(tm.indexes.begin(), tm.indexes.end());
    std::reverse(uuid.begin(), uuid.end());
    auto pmi = merge_memory_indexes((const IMemoryIndex **)&tm.indexes[0], tm.indexes.size());
    if (!pmi)
        LOG_ERROR_RETURN(0, nullptr, "failed to merge indexes");
    return pmi;
    // all indexes will be deleted automatically by ptr_vector
}

IFileRO *open_files_ro(IFile **files, size_t n, bool ownership) {
    if (n > MAX_STACK_LAYERS) {
        LOG_ERROR_RETURN(0, 0, "open too many files (` > `)", n, MAX_STACK_LAYERS);
    }
    if (!files || n == 0)
        return nullptr;

    HeaderTrailer ht;
    vector<IFile *> m_files(files, files + n);
    vector<UUID> m_uuid(n);
    auto pmi = load_merge_index(m_files, m_uuid, ht);
    if (!pmi)
        return nullptr;

    auto rst = new LSMTReadOnlyFile;
    rst->m_index = pmi;
    rst->m_files = move(m_files);
    rst->m_uuid = move(m_uuid);
    rst->m_vsize = ht.virtual_size;
    rst->m_file_ownership = ownership;

    LOG_DEBUG("open ` layers", n);
    for (int i = 0; i < (int)n; i++) {
        LOG_DEBUG(rst->m_uuid[i]);
    }
    return rst;
}

int merge_files_ro(vector<IFile *> files, const CommitArgs &args) {
    HeaderTrailer ht;
    vector<UUID> files_uuid(files.size());
    auto pmi = unique_ptr<IMemoryIndex>(load_merge_index(files, files_uuid, ht));
    if (!pmi)
        return -1;

    unique_ptr<SegmentMapping[]> ri(new SegmentMapping[pmi->size()]);
    memcpy(ri.get(), pmi->buffer(), sizeof(ri[0]) * pmi->size());
    unique_ptr<char[]> DISCARD_BLOCK(new char[ALIGNMENT]);

    atomic_uint64_t _no_use_var(0);
    CompactOptions opts;
    opts.src_files = (IFile **)&files[0];
    opts.n = files.size();
    opts.raw_index = ri.get();
    opts.index_size = pmi->size();
    opts.virtual_size = ht.virtual_size;
    opts.commit_args = &args;
    int ret = compact(opts, _no_use_var);
    return ret;
}

int merge_files_ro(IFile **src_files, size_t n, const CommitArgs &args) {
    if (!src_files || n == 0 || !args.as)
        LOG_ERROR_RETURN(EINVAL, -1, "invalid argument(s)");

    vector<IFile *> m_files(src_files, src_files + n);
    auto ret = merge_files_ro(m_files, args);
    return ret;
}

static bool verify_order(const vector<IFile *> &layers, const vector<UUID> &uuid, int start_layer) {
    UUID parent_uuid;
    parent_uuid.clear();
    for (int i = start_layer; i < (int)layers.size(); i++) {
        auto layer_uuid = uuid[i];
        LayerInfo args;
        if (load_layer_info((IFile **)&layers[i], 1, args))
            return false;
        if (parent_uuid.is_null() == false) {
            if (layer_uuid != parent_uuid) {
                LOG_ERROR_RETURN(0, false,
                                 "parent uuid mismatch in layer `: which UUID is: `, previous "
                                 "layer's UUID expected: `",
                                 i, layer_uuid, parent_uuid);
            }
        }
        if (i < (int)layers.size() - 1) {
            parent_uuid.parse(args.parent_uuid);
        }
    }
    return true;
}

IFileRW *stack_files(IFileRW *upper_layer, IFileRO *lower_layers, bool ownership,
                     bool check_order) {
    auto u = (LSMTFile *)upper_layer;
    auto l = (LSMTReadOnlyFile *)lower_layers;
    if (!u || u->m_files.size() != 1)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid upper layer");
    if (!l)
        return upper_layer;
    ALIGNED_MEM(buf, HeaderTrailer::SPACE, ALIGNMENT4K);
    auto pht = verify_ht(u->m_files[0], buf);
    if (pht == nullptr) {
        LOG_ERRNO_RETURN(0, nullptr, "verify upper layer's Header failed.");
    }
    auto idx = create_combo_index((IMemoryIndex0 *)u->m_index, l->m_index, true);
    LSMTFile *rst = new LSMTFile;
    rst->m_index = idx;
    rst->m_findex = u->m_findex;
    rst->m_vsize = u->m_vsize;
    rst->m_file_ownership = ownership;
    rst->m_files.reserve(1 + l->m_files.size());
    rst->m_uuid.reserve(1 + l->m_uuid.size());
    for (auto &x : l->m_files)
        rst->m_files.push_back(x);
    for (auto &x : l->m_uuid)
        rst->m_uuid.push_back(x);
    // check order of image ro layers.
    if (check_order) {
        if (verify_order(rst->m_files, rst->m_uuid, 1) == false)
            return nullptr;
    } else {
        LOG_WARN("STACK FILES WITHOUT CHECK ORDER!!!");
    }
    rst->m_files.insert(rst->m_files.begin(), u->m_files[0]);
    rst->m_uuid.insert(rst->m_uuid.begin(), u->m_uuid[0]);
    u->m_index = l->m_index = nullptr;
    l->m_file_ownership = u->m_file_ownership = false;
    if (ownership) {
        delete u;
        delete l;
    }
    return rst;
}

} // namespace LSMT