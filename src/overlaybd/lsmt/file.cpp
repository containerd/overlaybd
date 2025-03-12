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
#include <cstdint>
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
#include <sys/ioctl.h>
#include "index.h"
#include "photon/common/alog.h"
#include "photon/common/uuid.h"
#include "photon/fs/filesystem.h"
#include "photon/fs/virtual-file.h"
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/thread/thread.h>

#define PARALLEL_LOAD_INDEX 32

using namespace std;
using namespace photon::fs;

typedef atomic<uint64_t> atomic_uint64_t;

namespace LSMT {
LogBuffer &operator<<(LogBuffer &log, Segment s) {
    return log.printf("Segment[", s.offset + 0, ',', s.length + 0, ']');
}
LogBuffer &operator<<(LogBuffer &log, const SegmentMapping &m) {
    return log.printf((Segment &)m, "--> Mapping[", m.moffset + 0, ',', m.zeroed + 0, ',',
                      m.tag + 0, ']');
}
LogBuffer &operator<<(LogBuffer &log, const RemoteMapping &m) {
    return log.printf("[offset: `, count: `, remote_offset: `]", m.offset + 0, m.count + 0,
                      m.roffset + 0);
}

enum class LSMTFileType { RO, RW, SparseRW, WarpFileRO, WarpFile };

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
    static const uint32_t FLAG_SPARSE_RW = 4;    // 1:sparse file    0:normal file

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
    bool is_sparse_rw() const {
        return get_flag_bit(FLAG_SPARSE_RW);
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

    void set_sparse_rw() {
        set_flag_bit(FLAG_SPARSE_RW);
    }
    void clr_sparse_rw() {
        clr_flag_bit(FLAG_SPARSE_RW);
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
    uint16_t reserved;        // Reserved.

    static const uint8_t LSMT_V1 = 1;     // v1 (UUID check)
    static const uint8_t LSMT_SUB_V1 = 1; // .1 deprecated level range.

    uint8_t version = LSMT_V1;
    uint8_t sub_version = LSMT_SUB_V1;

    char user_tag[TAG_SIZE]{}; // 256B commit message.

} __attribute__((packed));

class LSMTReadOnlyFile;
static int merge_files_ro(vector<IFile *> files, const CommitArgs &args);
static LSMTReadOnlyFile *open_file_ro(IFile *file, bool ownership, bool reserve_tag);
static HeaderTrailer *verify_ht(IFile *file, char *buf, bool is_trailer = false,
                                ssize_t st_size = -1);


static const int ABORT_FLAG_DETECTED = -2;

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
    if (args.sparse_rw)
        pht->set_sparse_rw();
    else
        pht->clr_sparse_rw();

    pht->index_offset = index_offset;
    pht->index_size = index_size;
    pht->virtual_size = args.virtual_size;
    pht->set_uuid(args.uuid);
    pht->parent_uuid = args.parent_uuid;
    if (pht->set_tag(args.user_tag, args.len) != 0)
        return -1;
    if (is_header) {
        LOG_INFO("write header {virtual_size: `, uuid: `, parent_uuid: `}", args.virtual_size,
                 pht->uuid.c_str(), pht->parent_uuid.c_str());
    } else {
        LOG_INFO(
            "write trailer {index_offset: `, index_size: `, virtual_size: `, uuid: `, parent_uuid: `, sealed: `}",
            pht->index_offset + 0, pht->index_size + 0, pht->virtual_size + 0, pht->uuid.c_str(),
            pht->parent_uuid.c_str(), pht->is_sealed());
    }

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

    CompactOptions(const vector<IFile *> *files, SegmentMapping *mapping, size_t index_size,
                   size_t vsize, const CommitArgs *args)
        : src_files((IFile **)&(*files)[0]), n(files->size()), raw_index(mapping),
          index_size(index_size), virtual_size(vsize), commit_args(args) {
        LOG_INFO("generate compact options, file count: `", n);
    };
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
    layer.sparse_rw = pht->is_sparse_rw();
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
    layer.sparse_rw = false;
    layer.user_tag = commit_args->user_tag;
    layer.uuid.clear();
    if (UUID::String::is_valid((commit_args->uuid).c_str())) {
        layer.uuid.parse(commit_args->uuid);
    }
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
    LSMTFileType m_filetype = LSMTFileType::RO;

    virtual ~LSMTReadOnlyFile() {
        LOG_INFO("pread times: `, size: `M", lsmt_io_cnt, lsmt_io_size >> 20);
        close();
        if (m_file_ownership) {
            LOG_DEBUG("m_file_ownership:`, m_files.size:`", m_file_ownership, m_files.size());
            for (auto &x : m_files)
                safe_delete(x);
        }
    }

    virtual int vioctl(int request, va_list args) override {
        if (request == GetType) {
            return (int)m_filetype;
        }
        LOG_ERROR_RETURN(EINVAL, -1, "invaid request code");
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

    virtual std::vector<IFile *> get_lower_files() const override {
        return m_files;
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
            if (buf != nullptr) {
                (char *&)buf += MAX_IO_SIZE;
            }
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
                if (buf != nullptr) {
                    memset(buf, 0, step);
                    (char *&)buf += step;
                }
                return 0;
            },
            [&](const SegmentMapping &m) __attribute__((always_inline)) {
                if (m.tag >= m_files.size()) {
                    LOG_DEBUG(" ` >= `", m.tag, m_files.size());
                }
                assert(m.tag < m_files.size());
                ssize_t size = m.length * ALIGNMENT;
                // LOG_DEBUG("offset: `, length: `", m.moffset, size);
                ssize_t ret = m_files[m.tag]->pread(buf, size, m.moffset * ALIGNMENT);
                if (ret < size) {
                    if (ret < 0) {
                        LOG_ERRNO_RETURN(0, -1,
                                         "failed to read from `-th file ( ` pread return: ` < size: `)",
                                         m.tag, m_files[m.tag], ret, size);
                    }
                    size_t ret2 = m_files[m.tag]->pread((char *)buf + ret, size - ret, m.moffset * ALIGNMENT + ret);
                    if (ret2) {
                        LOG_ERRNO_RETURN(0, (int)ret,
                                         "failed to read from `-th file ( ` pread return: ` < size: `)",
                                         m.tag, m_files[m.tag], ret, size);
                    } else {
                        memset((char *)buf + ret, 0, size - ret);
                    }
                }
                lsmt_io_size += ret;
                lsmt_io_cnt++;
                (char *&)buf += size;
                return 0;
            });
        return (ret >= 0) ? nbytes : ret;
    }

    virtual IFile *front_file() {
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
    virtual IFileSystem *filesystem() override {
        auto file = front_file();
        if (!file)
            LOG_ERROR_RETURN(ENOSYS, nullptr, "no underlying files found!");
        return file->filesystem();
    }

    UNIMPLEMENTED(int update_vsize(size_t vsize) override);
    UNIMPLEMENTED(int close_seal(IFileRO **reopen_as = nullptr) override);

    // It can commit a RO file after close_seal()
    int commit(const CommitArgs &args) const override {
        if (m_files.size() > 1) {
            LOG_ERROR_RETURN(ENOTSUP, -1, "not supported: commit stacked files");
        }
        ALIGNED_MEM(buf, HeaderTrailer::SPACE, ALIGNMENT4K);
        // read file information in Trailer to check if it is a RW sealed file
        auto pht = verify_ht(m_files[0], buf, true);
        if (!pht->is_sealed()) {
            LOG_ERROR_RETURN(ENOTSUP, -1, "Commit a compacted LSMTReadonlyFile is not allowed.");
        }
        CompactOptions opts(&m_files, (SegmentMapping *)m_index->buffer(), m_index->size(), m_vsize,
                            &args);

        atomic_uint64_t _no_use_var(0);
        return compact(opts, _no_use_var);
    }

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

    virtual ssize_t seek_data(off_t begin, off_t end, vector<Segment> &segs) override {

        begin /= ALIGNMENT;
        end /= ALIGNMENT;
        while (begin < end) {
            SegmentMapping mappings[128];
            auto length = (end - begin < Segment::MAX_LENGTH ? end - begin : Segment::MAX_LENGTH);
            Segment s{(uint64_t)begin, (uint32_t)length};
            auto find = m_index->lookup(s, mappings, 128);
            if (find == 0) {
                begin+=length;
                continue;
            }
            segs.insert(segs.end(), mappings, mappings + find);
            begin=mappings[find-1].end();

        }
        return segs.size();
    }

    virtual int flatten(IFile *as) override{
        CommitArgs args(as);
        vector<IFile*> files = m_files;
        reverse(files.begin(), files.end());
        return merge_files_ro(files, args);
    }
};

class LSMTFile : public LSMTReadOnlyFile {
public:
    typedef photon::mutex Mutex;
    typedef photon::scoped_lock Lock;

    atomic_uint64_t m_compacted_idx_size; // count of compacted raw-index

    bool m_init_concurrency = false;
    uint64_t m_data_offset = HeaderTrailer::SPACE / ALIGNMENT;

    uint8_t m_rw_tag = 0;

    Mutex m_rw_mtx;
    IFile *m_findex = nullptr;

    vector<SegmentMapping> m_stacked_mappings;
    // used as a buffer for batch write (aka "group commit")
    uint32_t nmapping = 0;
    // # of elements in the mapping buffer

    LSMTFile() {
        m_compacted_idx_size.store(0);
        m_filetype = LSMTFileType::RW;
    }

    ~LSMTFile() {
        LOG_DEBUG(" ~LSMTFile()");
        close();
    }

    virtual IFile *front_file() override {
        if (m_files[m_rw_tag]) {
            return m_files[m_rw_tag];
        }
        return nullptr;
    }

    virtual int vioctl(int request, va_list args) override {
        if (request == GetType) {
            return LSMTReadOnlyFile::vioctl(request, args);
        }
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
        LOG_DEBUG("ownership:`, m_findex:`", m_file_ownership, m_findex);
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
                append(m_findex, &m, sizeof(m));
            } else {
                m_stacked_mappings[nmapping++] = m;
                if (nmapping == m_stacked_mappings.size() /* || TODO: timeout  */) {
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
            moffset = append(m_files[m_rw_tag], buf, count);
            if (moffset == 0)
                return -1;
            m_vsize = max(m_vsize, count + offset);
            if (m_vsize < count + offset) {
                LOG_INFO("resize m_visze: `->`", m_vsize, count + offset);
            }
            SegmentMapping m{
                (uint64_t)offset / (uint64_t)ALIGNMENT,
                (uint32_t)count / (uint32_t)ALIGNMENT,
                (uint64_t)moffset / (uint64_t)ALIGNMENT,
            };
            m.tag = m_rw_tag;
            assert(m.length > (uint32_t)0);
            m_data_offset = m.mend();
            static_cast<IMemoryIndex0 *>(m_index)->insert(m);
            append_index(m);
        }

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
        CHECK_ALIGNMENT(len, offset);
        SegmentMapping m{
            (uint64_t)offset / (uint64_t)ALIGNMENT,
            (uint32_t)len / (uint32_t)ALIGNMENT,
            0,
        };
        m.discard();
        return this->discard(m);
    }

    virtual int discard(SegmentMapping &m) {
        off_t pos = m_files[m_rw_tag]->lseek(0, SEEK_END);
        m.moffset = (uint64_t)(pos / ALIGNMENT);
        m.tag = m_rw_tag;
        LOG_DEBUG(m);
        static_cast<IMemoryIndex0 *>(m_index)->insert(m);
        Lock lock(m_rw_mtx);
        append_index(m);
        return 0;
    }

    int update_header_vsize(IFile *file, size_t vsize) {
        ALIGNED_MEM(buf, HeaderTrailer::SPACE, ALIGNMENT4K)
        if (file->pread(buf, HeaderTrailer::SPACE, 0) != HeaderTrailer::SPACE) {
            LOG_ERROR_RETURN(0, -1, "read layer header failed.");
        }
        HeaderTrailer *ht = (HeaderTrailer *)buf;
        ht->virtual_size = vsize;
        if (file->pwrite(buf, HeaderTrailer::SPACE, 0) != HeaderTrailer::SPACE) {
            LOG_ERROR_RETURN(0, -1, "write layer header failed.");
        }
        return 0;
    }

    virtual int update_vsize(size_t vsize) override {
        LOG_INFO("update vsize for LSMTFile ", VALUE(vsize));
        m_vsize = vsize;
        if (update_header_vsize(m_files[m_rw_tag], vsize) < 0) {
            LOG_ERROR_RETURN(0, -1, "failed to update data vsize");
        }
        if (update_header_vsize(m_findex, vsize) < 0) {
            LOG_ERROR_RETURN(0, -1, "failed to update index vsize");
        }
        return 0;
    }

    virtual int commit(const CommitArgs &args) const override {
        if (m_files.size() > 1) {
            LOG_ERROR_RETURN(ENOTSUP, -1, "not supported: commit stacked files");
        }

        auto m_index0 = (IMemoryIndex0 *)m_index;
        unique_ptr<SegmentMapping[]> mapping(m_index0->dump());
        CompactOptions opts(&m_files, mapping.get(), m_index->size(), m_vsize, &args);

        atomic_uint64_t _no_use_var(0);
        return compact(opts, _no_use_var);
    }

    virtual int close_seal(IFileRO **reopen_as = nullptr) override {
        auto m_index0 = (IMemoryIndex0 *)m_index;
        unique_ptr<SegmentMapping[]> mapping(m_index0->dump(ALIGNMENT));
        uint64_t index_offset = m_files[m_rw_tag]->lseek(0, SEEK_END);
        ssize_t index_bytes = m_index0->size() * sizeof(SegmentMapping);
        index_bytes = (index_bytes + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT;
        auto ret = m_files[m_rw_tag]->write(mapping.get(), index_bytes);
        if (ret < index_bytes)
            LOG_ERRNO_RETURN(0, -1, "failed to write index.");

        LayerInfo layer;
        if (load_layer_info(&m_files[m_rw_tag], 1, layer, true) != 0)
            return -1;
        ret = write_header_trailer(m_files[m_rw_tag], false, true, true, index_offset,
                                   m_index0->size(), layer);
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
            auto p = new LSMTReadOnlyFile;
            p->m_index = new_index;
            p->m_files = {m_files.back()};
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
        m_files[m_rw_tag]->fsync();
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
        auto ret = m_files[m_rw_tag]->fstat(&buf);
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

    virtual int flatten(IFile *as) override {

        unique_ptr<IComboIndex> pmi((IComboIndex*)(m_index->make_read_only_index()));
        CommitArgs args(as);
        atomic_uint64_t _no_use_var(0);
        CompactOptions opts(&m_files, (SegmentMapping*)(pmi->buffer()), pmi->size(), m_vsize, &args);
        return compact(opts, _no_use_var);
    }
};
class LSMTSparseFile : public LSMTFile {
public:
    static const off_t BASE_MOFFSET = HeaderTrailer::SPACE;

    LSMTSparseFile() {
        m_filetype = LSMTFileType::SparseRW;
    }

    virtual int close() override {
        return LSMTReadOnlyFile::close();
    }

    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        LOG_DEBUG("{offset:`,length:`}", offset, count);
        CHECK_ALIGNMENT(count, offset);

        while (count > MAX_IO_SIZE) {
            auto ret = pwrite(buf, MAX_IO_SIZE, offset);
            if (ret < (ssize_t)MAX_IO_SIZE)
                return -1;
            (char *&)buf += MAX_IO_SIZE;
            count -= MAX_IO_SIZE;
            offset += MAX_IO_SIZE;
        }
        auto moffset = BASE_MOFFSET + offset;
        SegmentMapping m{
            (uint64_t)offset / (uint64_t)ALIGNMENT,
            (uint32_t)count / (uint32_t)ALIGNMENT,
            (uint64_t)moffset / (uint64_t)ALIGNMENT,
        };
        m.tag = m_rw_tag;
        ssize_t ret = -1;
        {
            ret = m_files[m_rw_tag]->pwrite(buf, count, moffset);
            if (ret != (ssize_t)count) {
                LOG_ERRNO_RETURN(0, -1, "write failed, file:`, ret:`, pos:`, count:`",
                                 m_files[m_rw_tag], ret, moffset, count);
            }
            LOG_DEBUG("insert segment: `", m);
            static_cast<IMemoryIndex0 *>(m_index)->insert(m);
        }
        return ret;
    }

    // virtual int discard(off_t offset, off_t len) override
    virtual int discard(SegmentMapping &m) override {
        m.moffset = (uint64_t)(m.offset + (HeaderTrailer::SPACE / ALIGNMENT));
        LOG_DEBUG(m);
        static_cast<IMemoryIndex0 *>(m_index)->insert(m);
        return m_files[m_rw_tag]->trim(m.offset * ALIGNMENT + HeaderTrailer::SPACE,
                                       m.length * ALIGNMENT);
    }

    static int create_mappings(const IFile *file, vector<SegmentMapping> &mappings,
                               off_t base = BASE_MOFFSET) {

        auto moffset = base;
        while (true) {
            auto begin = const_cast<IFile *>(file)->lseek(moffset, SEEK_DATA);
            if (begin == -1)
                break;
            auto end = const_cast<IFile *>(file)->lseek(begin, SEEK_HOLE);
            if (end == -1)
                break;
            LOG_DEBUG("segment find: [ mbegin: `, mend: ` ]", begin, end);
            uint64_t total_length = (end - begin) / ALIGNMENT;
            uint64_t prev_offset = ((uint64_t)begin - base) / (uint64_t)ALIGNMENT;
            uint64_t prev_moffset = (uint64_t)(begin / ALIGNMENT);
            while (total_length > Segment::MAX_LENGTH) {
                uint32_t length = Segment::MAX_LENGTH;
                LOG_DEBUG("segment mapping {offset: `, length:`, moffset: `}", prev_offset, length,
                          prev_moffset);
                mappings.emplace_back(prev_offset, length, prev_moffset);
                prev_moffset += Segment::MAX_LENGTH;
                prev_offset += Segment::MAX_LENGTH;
                total_length -= Segment::MAX_LENGTH;
            }
            LOG_DEBUG("segment mapping {offset: `, length:`, moffset: `}", prev_offset,
                      total_length, prev_moffset);
            mappings.emplace_back(prev_offset, (uint32_t)total_length, prev_moffset);
            moffset = end;
        }
        if (errno != ENXIO) {
            LOG_ERRNO_RETURN(0, -1, "seek EOF failed, expected errno ENXIO(-6)");
        }
        LOG_INFO("segment size: `", mappings.size());
        return 0;
    }

    virtual int update_vsize(size_t vsize) override {
        LOG_INFO("update vsize for LSMTSparseFile ", VALUE(vsize));
        m_vsize = vsize;
        if (update_header_vsize(m_files[m_rw_tag], vsize) < 0) {
            LOG_ERROR_RETURN(0, -1, "failed to update data vsize");
        }
        m_files[m_rw_tag]->ftruncate(vsize + HeaderTrailer::SPACE);
        return 0;
    }
};

class LSMTWarpFile : public LSMTFile {
public:
    const static int READ_BUFFER_SIZE = 65536;
    IFile *m_target_file = nullptr;

    LSMTWarpFile() {
        m_filetype = LSMTFileType::WarpFile;
    }
    ~LSMTWarpFile() {
        if (m_file_ownership) {
            delete m_target_file;
        }
    }

    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        LOG_DEBUG("write fs meta {offset: `, len: `}", offset, count);
        auto tag = m_rw_tag + (uint8_t)SegmentType::fsMeta;
        SegmentMapping m{
            (uint64_t)offset / (uint64_t)ALIGNMENT,
            (uint32_t)count / (uint32_t)ALIGNMENT,
            (uint64_t)offset / (uint64_t)ALIGNMENT,
        };
        m.tag = tag;
        auto file = m_files[tag];
        LOG_DEBUG("insert segment: `, filePtr: `", m, file);
        auto ret = file->pwrite(buf, count, offset);
        if (ret != (ssize_t)count) {
            LOG_ERRNO_RETURN(0, -1, "write failed, file:`, ret:`, pos:`, count:`", file, ret,
                             offset, count);
        }
        static_cast<IMemoryIndex0 *>(m_index)->insert(m);
        append_index(m);
        return count;
    }

    virtual int vioctl(int request, va_list args) override {
        if (request == GetType) {
            return LSMTReadOnlyFile::vioctl(request, args);
        }
        if (request != RemoteData) {
            LOG_ERROR_RETURN(EINVAL, -1, "invaid request code");
        }
        va_list tmp;
        va_copy(tmp, args);
        auto lba = va_arg(tmp, RemoteMapping);
        va_end(tmp);
        LOG_DEBUG("RemoteMapping: {offset: `, count: `, roffset: `}", lba.offset, lba.count,
                  lba.roffset);
        size_t nwrite = 0;
        while (lba.count > 0) {
            SegmentMapping m;
            m.offset = lba.offset / ALIGNMENT;
            m.length = (Segment::MAX_LENGTH < lba.count / ALIGNMENT ? Segment::MAX_LENGTH
                                                                    : lba.count / ALIGNMENT);
            m.moffset = lba.roffset / ALIGNMENT;
            m.tag = m_rw_tag + (uint8_t)SegmentType::remoteData;
            LOG_DEBUG("insert segment: ` into findex: `", m, m_findex);
            static_cast<IMemoryIndex0 *>(m_index)->insert(m);
            append_index(m);
            nwrite += m.length * ALIGNMENT;
            lba.offset += m.length * ALIGNMENT;
            lba.count -= m.length * ALIGNMENT;
            lba.roffset += m.length * ALIGNMENT;
        }
        return nwrite;
    }

    size_t compact(CompactOptions &opts, size_t moffset, size_t &nindex) const {

        auto dest_file = opts.commit_args->as;
        auto marray = ptr_array(&opts.raw_index[0], opts.index_size);
        vector<SegmentMapping> compact_index;
        size_t compacted_idx_size = 0;
        moffset /= ALIGNMENT;
        for (auto &m : marray) {
            if (m.tag == (uint8_t)SegmentType::remoteData) {
                compact_index.push_back(m);
                compacted_idx_size++;
                continue;
            }
            compacted_idx_size++;
            if (m.zeroed) {
                m.moffset = moffset;
                compact_index.push_back(m);
                // there is no need do pcopy if current block is zero-marked.
                continue;
            }
            auto ret = pcopy(opts, m, moffset, compact_index);
            if (ret < 0)
                return (int)ret;
            moffset += ret;
        }
        uint64_t index_offset = moffset * ALIGNMENT;
        auto index_size = compress_raw_index(&compact_index[0], compact_index.size());
        LOG_DEBUG("write index to dest_file `, offset: `, size: `*`", dest_file, index_offset,
                  index_size, sizeof(SegmentMapping));
        auto nwrite = dest_file->write(&compact_index[0], index_size * sizeof(SegmentMapping));
        if (nwrite != (ssize_t)(index_size * sizeof(SegmentMapping))) {
            LOG_ERRNO_RETURN(0, -1, "write index failed");
        }
        nindex = index_size;
        // auto pos = dest_file->lseek(0, SEEK_END);
        auto pos = index_offset + index_size * sizeof(SegmentMapping);
        LOG_INFO("write index done, file_size: `", pos);
        return pos;
    }

    int commit(const CommitArgs &args) const override {

        auto m_index0 = (IMemoryIndex0 *)m_index;
        unique_ptr<SegmentMapping[]> mapping(m_index0->dump());
        CompactOptions opts(&m_files, mapping.get(), m_index->size(), m_vsize, &args);
        LayerInfo info;
        info.virtual_size = m_vsize;
        info.uuid.clear();
        if (UUID::String::is_valid((args.uuid).c_str())) {
            LOG_INFO("set UUID: `", args.uuid.data);
            info.uuid.parse(args.uuid);
        }
        if (UUID::String::is_valid((args.parent_uuid).c_str())) {
            LOG_INFO("set parent UUID: `", args.parent_uuid.data);
            info.parent_uuid.parse(args.parent_uuid);
        }
        write_header_trailer(args.as, true, true, true, 0, 0, info);
        size_t index_size = 0, index_offset = 0;
        auto ret = compact(opts, HeaderTrailer::SPACE, index_size);
        if (ret < 0) {
            LOG_ERRNO_RETURN(0, -1, "compact data failed.");
        }
        index_offset = ret - index_size * sizeof(SegmentMapping);
        LOG_INFO("compact data success, dest_file size: `", ret);
        write_header_trailer(args.as, false, true, true, index_offset, index_size, info);
        return 0;
    }
};

static HeaderTrailer *verify_ht(IFile *file, char *buf, bool is_trailer, ssize_t st_size) {
    if (file == nullptr) {
        LOG_ERRNO_RETURN(0, nullptr, "invalid file ptr (null).");
    }
    auto pht = (HeaderTrailer *)buf;
    if (!is_trailer) {
        auto ret = file->pread(buf, HeaderTrailer::SPACE, 0);
        if (ret < (ssize_t)HeaderTrailer::SPACE)
            LOG_ERRNO_RETURN(0, nullptr, "failed to read file header.");

        if (!pht->verify_magic() || !pht->is_header())
            LOG_ERROR_RETURN(0, nullptr, "header magic/type don't match");
        return pht;
    }
    if (st_size == -1) {
        struct stat st;
        file->fstat(&st);
        st_size = st.st_size;
    }
    auto trailer_offset = st_size - HeaderTrailer::SPACE;
    auto ret = file->pread(buf, HeaderTrailer::SPACE, trailer_offset);
    if (ret < (ssize_t)HeaderTrailer::SPACE)
        LOG_ERRNO_RETURN(0, nullptr, "failed to read file trailer.");
    if (!pht->verify_magic() || !pht->is_trailer() || !pht->is_data_file() || !pht->is_sealed())
        LOG_ERROR_RETURN(0, nullptr,
                         "trailer magic, trailer type, "
                         "file type or sealedness doesn't match");
    return pht;
}

static SegmentMapping *do_load_index(IFile *file, HeaderTrailer *pheader_trailer, bool trailer,
                                     uint8_t warp_file_tag = 0) {

    ALIGNED_MEM(buf, HeaderTrailer::SPACE, ALIGNMENT4K);
    auto pht = verify_ht(file, buf);
    if (pht == nullptr) {
        return nullptr;
    }
    struct stat stat;
    auto ret = file->fstat(&stat);
    if (ret < 0)
        LOG_ERRNO_RETURN(0, nullptr, "failed to stat file.");
    assert(pht->is_sparse_rw() == false);
    uint64_t index_bytes;
    if (trailer) {
        if (!pht->is_data_file())
            LOG_ERROR_RETURN(0, nullptr, "uncognized file type");
        pht = verify_ht(file, buf, true, stat.st_size);
        if (pht == nullptr) {
            return nullptr;
        }
        auto trailer_offset = stat.st_size - HeaderTrailer::SPACE;
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
    uint8_t min_tag = 255;
    for (size_t i = 0; i < pht->index_size; ++i) {
        if (ibuf[i].offset != SegmentMapping::INVALID_OFFSET) {
            ibuf[index_size] = ibuf[i];
            ibuf[index_size].tag = (warp_file_tag ? ibuf[i].tag : 0);
            if (min_tag > ibuf[index_size].tag)
                min_tag = ibuf[index_size].tag;
            index_size++;
        }
    }
    if (warp_file_tag) {
        LOG_INFO("rebuild index tag for LSMTWarpFile.");
        for (size_t i = 0; i < index_size; i++) {
            if (warp_file_tag == 1) /* only fsmeta */
                ibuf[i].tag = (uint8_t)SegmentType::fsMeta;
            if (warp_file_tag == 2) /* only remote data */
                ibuf[i].tag = (uint8_t)SegmentType::remoteData;
            if (warp_file_tag == 3) /* only remote data */
                ibuf[i].tag -= min_tag;
            LOG_DEBUG("`", ibuf[i]);
        }
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
    auto rst = new LSMTReadOnlyFile;
    rst->m_index = pi;
    rst->m_files = {file};
    rst->m_uuid.resize(1);
    rst->m_uuid[0].parse(ht.uuid);
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
    if ((pht == nullptr) || ((pht->is_sparse_rw() == false) && (!findex))) {
        LOG_ERRNO_RETURN(0, nullptr, "invalid file ptr, fdata: ` findex: `", fdata, findex);
    }
    struct stat stat;
    int ret = fdata->fstat(&stat);
    if (ret < 0) {
        LOG_ERRNO_RETURN(0, nullptr, "failed to stat data file.");
    }
    IMemoryIndex0 *pi = nullptr;
    if (pht->is_sparse_rw() == false) {
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
    } else {
        LOG_DEBUG("create index from sparse file.");
        vector<SegmentMapping> mappings;
        if (LSMTSparseFile::create_mappings(fdata, mappings) == -1) {
            LOG_ERROR_RETURN(0, nullptr, "failed to create segment mappings from sparse file!");
        }
        pi = create_memory_index0((const SegmentMapping *)&mappings[0], mappings.size(),
                                  HeaderTrailer::SPACE / ALIGNMENT, stat.st_size / ALIGNMENT);
        if (!pi) {
            LOG_ERROR_RETURN(0, nullptr, "failed to create memory index from sparse file!");
        }
    }
    LSMTFile *rst = nullptr;
    if (pht->is_sparse_rw() == false) {
        LOG_INFO("create LSMTFile object (append-only)");
        rst = new LSMTFile;
    } else {
        LOG_INFO("create LSMTSparseFile object");
        rst = new LSMTSparseFile;
    }
    rst->m_index = pi;
    rst->m_findex = findex;
    rst->m_files.push_back(fdata);
    rst->m_vsize = pht->virtual_size;
    rst->m_file_ownership = ownership;
    UUID raw;
    raw.parse(pht->uuid);
    rst->m_uuid.push_back(raw);
    LOG_INFO("Layer Info: { UUID:` , Parent_UUID: `, SparseRW: `, Virtual size: `, Version: `.` }",
             pht->uuid, pht->parent_uuid, pht->is_sparse_rw(), rst->m_vsize, pht->version,
             pht->sub_version);
    return rst;
}

IFileRW *create_file_rw(const LayerInfo &args, bool ownership) {
    auto fdata = args.fdata;
    auto findex = args.findex;
    if ((args.sparse_rw == false) && (!fdata || !findex)) {
        LOG_ERROR_RETURN(0, nullptr, "invalid file ptr, fdata: `, findex: `", fdata, findex);
    }
    LSMTFile *rst = nullptr;
    if (args.sparse_rw == false) {
        rst = new LSMTFile;
    } else {
        rst = new LSMTSparseFile;
    }
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
    if (!args.sparse_rw) {
        write_header_trailer(findex, true, false, false, HeaderTrailer::SPACE, 0, args);
    }
    HeaderTrailer tmp;
    // args.parent_uuid.to_string(parent_uuid, UUID::String::LEN);
    LOG_INFO("Layer Info: { UUID:`, Parent_UUID: `, Sparse: ` Virtual size: `, Version: `.` }", raw,
             args.parent_uuid, args.sparse_rw, rst->m_vsize, tmp.version, tmp.sub_version);
    if (args.sparse_rw) {
        fdata->ftruncate(args.virtual_size + HeaderTrailer::SPACE);
    }
    return rst;
}

IFileRW *create_warpfile(WarpFileArgs &args, bool ownership) {
    auto rst = new LSMTWarpFile();
    rst->m_findex = args.findex;
    LayerInfo info;
    info.sparse_rw = false;
    info.virtual_size = args.virtual_size;
    info.parent_uuid.parse(args.parent_uuid);
    info.uuid.parse(args.uuid);
    write_header_trailer(rst->m_findex, true, false, false, HeaderTrailer::SPACE, 0, info);
    rst->m_index = create_memory_index0((const SegmentMapping *)nullptr, 0, 0, 0);
    rst->m_files.resize(2);
    rst->m_files[(uint8_t)SegmentType::fsMeta] = args.fsmeta;
    rst->m_files[(uint8_t)SegmentType::remoteData] = args.target_file;
    rst->m_vsize = args.virtual_size;
    rst->m_file_ownership = ownership;
    UUID raw;
    raw.parse(args.uuid);
    rst->m_uuid.push_back(raw);
    HeaderTrailer tmp;
    tmp.version = 2;
    tmp.sub_version = 0;
    args.fsmeta->ftruncate(args.virtual_size);
    LOG_INFO("WarpImage Layer: { UUID:`, Parent_UUID: `, Virtual size: `, Version: `.` }", raw,
             info.parent_uuid, rst->m_vsize, tmp.version, tmp.sub_version);
    return rst;
}

IFileRW *open_warpfile_rw(IFile *findex, IFile *fsmeta_file, IFile *target_file, bool ownership) {
    // TODO empty fsmeta or remoteData
    auto rst = new LSMTWarpFile;
    rst->m_files.resize(2);
    LSMT::HeaderTrailer ht;
    auto p = do_load_index(findex, &ht, false, 3);
    auto pi = create_memory_index0(p, ht.index_size, 0, -1);
    if (!pi) {
        delete[] p;
        LOG_ERROR_RETURN(0, nullptr, "failed to create memory index!");
    }
    rst->m_index = pi;
    rst->m_findex = findex;
    rst->m_files = {fsmeta_file, target_file};
    rst->m_uuid.resize(1);
    rst->m_uuid[0].parse(ht.uuid);
    rst->m_vsize = ht.virtual_size;
    rst->m_file_ownership = ownership;
    LOG_INFO("Layer Info: { UUID: `, Parent_UUID: `, Virtual size: `, Version: `.` }", ht.uuid,
             ht.parent_uuid, rst->m_vsize, ht.version, ht.sub_version);
    return rst;
};

IFileRO *open_warpfile_ro(IFile *warpfile, IFile *target_file, bool ownership) {
    if (!warpfile || !target_file) {
        LOG_ERROR("invalid file ptr. file: `, `", warpfile, target_file);
        return nullptr;
    }
    HeaderTrailer ht;
    auto p = do_load_index(warpfile, &ht, true, 3);
    if (!p)
        LOG_ERROR_RETURN(EIO, nullptr, "failed to load index from file.");
    auto pi = create_memory_index(p, ht.index_size, 0, -1);
    if (!pi) {
        delete[] p;
        LOG_ERROR_RETURN(0, nullptr, "failed to create memory index!");
    }
    auto rst = new LSMTReadOnlyFile;
    rst->m_filetype = LSMTFileType::WarpFileRO;
    rst->m_index = pi;
    rst->m_files = {warpfile, target_file};
    rst->m_uuid.resize(1);
    rst->m_uuid[0].parse(ht.uuid);
    rst->m_vsize = ht.virtual_size;
    rst->m_file_ownership = ownership;
    LOG_INFO("Layer Info: { UUID: `, Parent_UUID: `, Virtual size: `, Version: `.` }", ht.uuid,
             ht.parent_uuid, rst->m_vsize, ht.version, ht.sub_version);
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

SegmentMapping *copy_lsmt_index(IFile *file, HeaderTrailer &ht) {
    auto lsmtfile = (IFileRO *)file;
    auto n = lsmtfile->index()->size();
    auto nbytes = n * sizeof(SegmentMapping);
    ht.index_size = n;
    struct stat st;
    lsmtfile->fstat(&st);
    ht.virtual_size = st.st_size;
    ht.index_offset = -1;
    UUID uu;
    ((IFileRO *)file)->get_uuid(uu);
    ht.set_uuid(uu);
    auto p = new SegmentMapping[nbytes];
    memcpy(p, lsmtfile->index()->buffer(), nbytes);
    return p;
}

void *do_parallel_load_index(void *param) {
    parallel_load_task *tm = (parallel_load_task *)param;
    while (true) {
        auto job = tm->get_job();
        if (job == nullptr || tm->eno != 0) {
            // error occured from another threads.
            return nullptr;
        }
        auto file = job->get_file();
        LOG_INFO("check `-th file is normal file or LSMT file", job->i);
        IMemoryIndex *pi = nullptr;
        LSMT::SegmentMapping *p = nullptr;
        auto type = file->ioctl(IFileRO::GetType);
        auto verify_begin = HeaderTrailer::SPACE / ALIGNMENT;
        if (type != -1) {
            LOG_INFO("LSMTFileType of file ` is `.", file, type);
            // copy idx
            p = copy_lsmt_index(file, job->ht);
            LOG_INFO("copy index and reset tag, count: `", (int)(job->ht.index_size));
            for (auto m = p; m < p + job->ht.index_size; m++) {
                LOG_DEBUG("`", *m);
                m->tag = 0;
                m->moffset = m->offset;
            }
            verify_begin = 0;

        } else {
            p = do_load_index(job->get_file(), &job->ht, true);
            if (!p) {
                job->set_error(EIO);
                LOG_ERROR_RETURN(0, nullptr, "failed to load index from `-th file", job->i);
            }
        }
        pi = create_memory_index(p, job->ht.index_size, verify_begin,
                                 job->ht.index_offset / ALIGNMENT);
        if (!pi) {
            delete[] p;
            job->set_error(EIO);
            LOG_ERROR_RETURN(0, nullptr, "failed to create memory index!");
        }
        job->set_index(pi);
        LOG_INFO("load index from `-th file done", job->i);
    }
    return NULL;
}

static IMemoryIndex *load_merge_index(vector<IFile *> &files, vector<UUID> &uuid, uint64_t &vsize) {
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
    for (auto it = tm.jobs.rbegin(); it != tm.jobs.rend(); ++it) {
        if ((*it).ht.virtual_size > 0) {
            vsize = (*it).ht.virtual_size;
            break;
        }
    }

    std::reverse(files.begin(), files.end());
    std::reverse(tm.indexes.begin(), tm.indexes.end());
    std::reverse(uuid.begin(), uuid.end());
    auto pmi = merge_memory_indexes((const IMemoryIndex **)&tm.indexes[0], tm.indexes.size());
    if (!pmi)
        LOG_ERROR_RETURN(0, nullptr, "failed to merge indexes");
    return pmi;
}

IFileRO *open_files_ro(IFile **files, size_t n, bool ownership) {
    if (n > MAX_STACK_LAYERS) {
        LOG_ERROR_RETURN(0, 0, "open too many files (` > `)", n, MAX_STACK_LAYERS);
    }
    if (!files || n == 0)
        return nullptr;

    uint64_t vsize;
    vector<IFile *> m_files(files, files + n);
    vector<UUID> m_uuid(n);
    auto pmi = load_merge_index(m_files, m_uuid, vsize);
    if (!pmi)
        return nullptr;

    auto rst = new LSMTReadOnlyFile;
    rst->m_index = pmi;
    rst->m_files = move(m_files);
    rst->m_uuid = move(m_uuid);
    rst->m_vsize = vsize;
    rst->m_file_ownership = ownership;

    LOG_DEBUG("open ` layers", n);
    for (int i = 0; i < (int)n; i++) {
        LOG_DEBUG("layer `, uuid `", i, rst->m_uuid[i]);
    }
    return rst;
}

static int merge_files_ro(vector<IFile *> files, const CommitArgs &args) {
    uint64_t vsize;
    vector<UUID> files_uuid(files.size());
    auto pmi = unique_ptr<IMemoryIndex>(load_merge_index(files, files_uuid, vsize));
    if (!pmi)
        return -1;

    unique_ptr<SegmentMapping[]> ri(new SegmentMapping[pmi->size()]);
    memcpy(ri.get(), pmi->buffer(), sizeof(ri[0]) * pmi->size());
    unique_ptr<char[]> DISCARD_BLOCK(new char[ALIGNMENT]);

    atomic_uint64_t _no_use_var(0);
    CompactOptions opts(&files, ri.get(), pmi->size(), vsize, &args);
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
    if (!u)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid upper layer");
    if (!l)
        return upper_layer;

    auto type = u->ioctl(IFileRO::GetType);
    LSMTFile *rst = nullptr;
    IComboIndex *idx = nullptr;
    int delta = 1;
    if (type != (uint8_t)LSMTFileType::WarpFile) {
        ALIGNED_MEM(buf, HeaderTrailer::SPACE, ALIGNMENT4K);
        auto pht = verify_ht(u->m_files[0], buf);
        if (pht == nullptr) {
            LOG_ERRNO_RETURN(0, nullptr, "verify upper layer's Header failed.");
        }
        if (!pht->is_sparse_rw()) {
            rst = new LSMTFile;
        } else {
            rst = new LSMTSparseFile;
        }
        // TODO: also for LSMTWarpFile
        if (u->m_vsize == 0) {
            if (u->update_vsize(l->m_vsize) < 0) {
                LOG_ERRNO_RETURN(0, nullptr, "failed to update vsize");
            }
        }
    } else {
        rst = new LSMTWarpFile;
        delta++;
    }
    idx = create_combo_index((IMemoryIndex0 *)u->m_index, l->m_index, l->m_files.size(), ownership);
    rst->m_index = idx;
    rst->m_findex = u->m_findex;
    rst->m_vsize = u->m_vsize;
    rst->m_file_ownership = ownership;
    rst->m_files.reserve(delta + l->m_files.size());
    rst->m_uuid.reserve(1 + l->m_uuid.size());
    for (auto &x : l->m_files)
        rst->m_files.push_back(x);
    for (auto &x : l->m_uuid)
        rst->m_uuid.push_back(x);
    // check order of image ro layers.
    if (check_order) {
        if (verify_order(rst->m_files, rst->m_uuid, 1) == false)
            return nullptr;
        LOG_INFO("check layer's parent uuid success.");
    }
    rst->m_files.push_back(u->m_files[0]);
    if (type == (uint8_t)LSMTFileType::WarpFile) {
        rst->m_files.push_back(u->m_files[1]);
    }
    rst->m_uuid.push_back(u->m_uuid[0]);
    rst->m_rw_tag = rst->m_files.size() - delta;
    if (ownership) {
        u->m_index = l->m_index = nullptr;
        l->m_file_ownership = u->m_file_ownership = false;
        delete u;
        delete l;
    }
    return rst;
}

IMemoryIndex *open_file_index(IFile *file) {
    HeaderTrailer ht;
    auto p = do_load_index(file, &ht, true);
    if (!p) {
        LOG_ERROR_RETURN(0, nullptr, "failed to load index");
    }

    auto pi = create_memory_index(p, ht.index_size, HeaderTrailer::SPACE / ALIGNMENT,
                                  ht.index_offset / ALIGNMENT, true, ht.virtual_size);
    if (!pi) {
        delete[] p;
        LOG_ERROR_RETURN(0, nullptr, "failed to create memory index");
    }
    return pi;
}

IFileRO *open_files_with_merged_index(IFile **src_files, size_t n, IMemoryIndex *index,
                                      bool ownership) {
    vector<IFile *> m_files(src_files, src_files + n);
    auto rst = new LSMTReadOnlyFile;
    rst->m_index = index;
    rst->m_files = move(m_files);
    rst->m_vsize = index->vsize();
    rst->m_uuid.resize(rst->m_files.size());
    rst->m_file_ownership = ownership;
    return rst;
}

int is_lsmt(IFile *file) {
    char buf[HeaderTrailer::SPACE];
    auto ret = file->pread(buf, HeaderTrailer::SPACE, 0);
    if (ret < (ssize_t)HeaderTrailer::SPACE)
        LOG_ERRNO_RETURN(0, -1, "failed to read file header.");
    auto pht = (HeaderTrailer *)buf;
    if (!pht->verify_magic() || !pht->is_header()) {
        LOG_DEBUG("file: ` is not lsmt object", file);
        return 1;
    }
    LOG_DEBUG("file: ` is lsmt object", file);
    return 0;
}

} // namespace LSMT
