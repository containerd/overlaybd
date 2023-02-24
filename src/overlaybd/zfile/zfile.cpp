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

#include "zfile.h"
#include <vector>
#include <algorithm>
#include <memory>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <photon/common/utility.h>
#include <photon/common/uuid.h>
#include <photon/fs/virtual-file.h>
#include <photon/fs/forwardfs.h>
#include "crc32/crc32c.h"
#include "compressor.h"

using namespace photon::fs;

#ifdef BENCHMARK
bool io_test = false;
uint32_t zfile_io_cnt = 0;
uint64_t zfile_io_size = 0;
uint64_t zfile_blk_cnt = 0;
#endif

namespace ZFile {

const static size_t BUF_SIZE = 512;
const static uint32_t NOI_WELL_KNOWN_PRIME = 100007;

template <typename T>
static std::unique_ptr<T[]> new_align_mem(size_t _size, size_t alignment = ALIGNMENT_4K) {
    size_t size = (_size + alignment - 1) / alignment * alignment;
    return std::unique_ptr<T[]>(new T[size]);
}

inline uint32_t crc32c(void *buf, size_t size) {
    return crc32::crc32c_extend(buf, size, NOI_WELL_KNOWN_PRIME);
}
/* ZFile Format:
    | Header (512B) | dict (optional) | compressed block 0 [checksum0] | compressed block 1
   [checksum1] | ... | compressed block N [checksumN] | jmp_table(index) | Trailer (512 B)|
*/
class CompressionFile : public VirtualReadOnlyFile {
public:
    struct HeaderTrailer {
        static const uint32_t SPACE = 512;

        static uint64_t MAGIC0() {
            static char magic0[] = "ZFile\0\1";
            return *(uint64_t *)magic0;
        }

        static constexpr UUID MAGIC1() {
            return {0x696a7574, 0x792e, 0x6679, 0x4140, {0x6c, 0x69, 0x62, 0x61, 0x62, 0x61}};
        }

        // offset 0, 8
        uint64_t magic0 = MAGIC0();
        UUID magic1 = MAGIC1();
        bool verify_magic() const {
            return magic0 == MAGIC0() && magic1 == MAGIC1();
        }

        // offset 24, 28, 32
        uint32_t size = sizeof(HeaderTrailer);
        uint32_t __padding = 0;
        uint64_t flags;

        static const uint32_t FLAG_SHIFT_HEADER = 0; // 1:header     0:trailer
        static const uint32_t FLAG_SHIFT_TYPE = 1;   // 1:data file, 0:index file
        static const uint32_t FLAG_SHIFT_SEALED = 2; // 1:YES,       0:NO
        static const uint32_t FLAG_SHIFT_HEADER_OVERWRITE = 3;

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
        bool is_header_overwrite() const {
            return get_flag_bit(FLAG_SHIFT_HEADER_OVERWRITE);
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
        void set_header_overwrite() {
            set_flag_bit(FLAG_SHIFT_HEADER_OVERWRITE);
        }

        void set_compress_option(const CompressOptions &opt) {
            this->opt = opt;
        }

        // offset 40, 48, 56, 64
        uint64_t index_offset; // in bytes
        uint64_t index_size;   // # of SegmentMappings
        uint64_t raw_data_size;
        uint64_t reserved_0;
        // offset 72
        CompressOptions opt;

    } /* __attribute__((packed)) */;
    static_assert(sizeof(HeaderTrailer) == 96, "sizeof(HeaderTrailer) != 96");

    struct JumpTable {
        typedef uint16_t uinttype;
        static const uinttype uinttype_max = UINT16_MAX;
        int group_size;

        std::vector<uint64_t> partial_offset;
        std::vector<uinttype> deltas; // partial sum in each page;

        off_t operator[](size_t idx) const {
            // return BASE  + deltas[ idx % (page_size) ]
            off_t part_idx = idx / group_size;
            off_t inner_idx = idx & (group_size - 1);
            auto part_offset = partial_offset[part_idx];
            if (inner_idx) {
                return part_offset + deltas[idx];
            }
            return part_offset;
        }

        size_t size() const {
            return deltas.size();
        }

        int build(const uint32_t *ibuf, size_t n, off_t offset_begin, uint32_t block_size) {
            group_size = (uinttype_max + 1) / block_size;
            partial_offset.reserve(n / group_size + 1);
            deltas.reserve(n + 1);
            auto raw_offset = offset_begin;
            partial_offset.push_back(raw_offset);
            deltas.push_back(0);
            for (ssize_t i = 1; i < (ssize_t)n + 1; i++) {
                raw_offset += ibuf[i - 1];
                if ((i % group_size) == 0) {
                    partial_offset.push_back(raw_offset);
                    deltas.push_back(0);
                    continue;
                }
                assert((uint64_t)deltas[i - 1] + ibuf[i - 1] < (uint64_t)uinttype_max);
                deltas.push_back(deltas[i - 1] + ibuf[i - 1]);
            }
            LOG_INFO("create jump table done. {part_count: `, deltas_count: `, size: `}",
                     partial_offset.size(), deltas.size(),
                     deltas.size() * sizeof(deltas[0]) +
                         partial_offset.size() * sizeof(partial_offset[0]));
            return 0;
        }

    } m_jump_table;

    HeaderTrailer m_ht;
    IFile *m_file = nullptr;
    std::unique_ptr<ICompressor> m_compressor;
    bool m_ownership = false;
    bool valid = false;

    CompressionFile(IFile *file, bool ownership) : m_file(file), m_ownership(ownership){};

    ~CompressionFile() {
        if (m_ownership) {
            delete m_file;
        }
    }

    UNIMPLEMENTED_POINTER(IFileSystem *filesystem() override);

    virtual int close() override {
        valid = false;
        return 0;
    }

    virtual int fstat(struct stat *buf) override {
        if (!valid) {
            LOG_ERROR_RETURN(EBADF, -1, "object invalid.");
        }
        auto ret = m_file->fstat(buf);
        if (ret != 0)
            return ret;
        buf->st_size = m_ht.raw_data_size;
        return ret;
    }

    class BlockReader {
    public:
        BlockReader(){};
        BlockReader(const CompressionFile *zfile, size_t offset, size_t count)
            : m_zfile(zfile), m_offset(offset) /* , m_count(count)  */
        {
            m_verify = zfile->m_ht.opt.verify;
            m_block_size = zfile->m_ht.opt.block_size; //+ m_verify * sizeof(uint32_t);
            m_begin_idx = m_offset / m_block_size;
            m_idx = m_begin_idx;
            m_end = m_offset + count - 1;
            m_end_idx = (m_offset + count - 1) / m_block_size + 1;
        }

        int reload(size_t idx) {
            auto read_size = get_blocks_length(idx, idx + 1);
            auto begin_offset = m_zfile->m_jump_table[idx];
            int trim_res = m_zfile->m_file->trim(begin_offset, read_size);
            if (trim_res < 0) {
                LOG_ERROR_RETURN(0, -1, "failed to trim block idx: `", idx);
            }
            auto readn = m_zfile->m_file->pread(m_buf + m_buf_offset, read_size, begin_offset);
            if (readn != (ssize_t)read_size) {
                LOG_ERRNO_RETURN(0, -1, "read compressed blocks failed. (offset: `, len: `)",
                                 begin_offset, read_size);
            }
            return 0;
        }

        int read_blocks(size_t begin, size_t end) {
            auto read_size = std::min(MAX_READ_SIZE, get_blocks_length(begin, end));
            auto begin_offset = m_zfile->m_jump_table[begin];
            auto readn = m_zfile->m_file->pread(m_buf, read_size, begin_offset);
            if (readn != (ssize_t)read_size) {
                LOG_ERRNO_RETURN(0, -1, "read compressed blocks failed. (offset: `, len: `)",
                                 begin_offset, read_size);
            }
            return 0;
        }

        __attribute__((always_inline)) size_t get_blocks_length(size_t begin, size_t end) const {
            assert(begin <= end);
            return m_zfile->m_jump_table[end] - m_zfile->m_jump_table[begin];
        }

        __attribute__((always_inline)) size_t get_buf_offset(size_t idx) {
            return get_blocks_length(m_begin_idx, idx);
        }

        __attribute__((always_inline)) bool buf_exceed(size_t idx) {
            return get_blocks_length(m_begin_idx, idx + 1) > sizeof(m_buf);
        }

        __attribute__((always_inline)) size_t get_inblock_offset(size_t offset) {
            return offset % m_block_size;
        }

        __attribute__((always_inline)) size_t block_size() const {
            return m_block_size;
        }

        __attribute__((always_inline)) size_t compressed_size() const {
            return get_blocks_length(m_idx, m_idx + 1) - (m_verify ? sizeof(uint32_t) : 0);
        }

        __attribute__((always_inline)) uint32_t crc32_code() const {
            if (!m_verify) {
                LOG_WARN("crc32 not support.");
                return -1;
            }
            return *(uint32_t *)&(m_buf[m_buf_offset + compressed_size()]);
        }

        struct iterator {
            size_t compressed_size;
            off_t cp_begin;
            size_t cp_len;
            iterator(BlockReader *reader) : m_reader(reader) {
                get_current_block();
            }
            iterator() {
            }

            const iterator operator*() {
                return *this;
            };

            const unsigned char *buffer() const {
                return (m_reader->m_buf + m_reader->m_buf_offset);
            }

            const uint32_t crc32_code() const {
                return m_reader->crc32_code();
            }

            void get_current_block() {
                m_reader->m_buf_offset = m_reader->get_buf_offset(m_reader->m_idx);

                auto blk_idx = m_reader->m_idx;
                compressed_size = m_reader->compressed_size();

                if (blk_idx == m_reader->m_begin_idx) {
                    cp_begin = m_reader->get_inblock_offset(m_reader->m_offset);
                    m_reader->m_offset = 0;
                } else {
                    cp_begin = 0;
                }
                cp_len = m_reader->block_size();
                if (blk_idx == m_reader->m_end_idx - 1) {
                    cp_len = m_reader->get_inblock_offset(m_reader->m_end) + 1;
                }
                cp_len -= cp_begin;
            }

            iterator &operator++() {
                m_reader->m_idx++;
                if (m_reader->m_idx == m_reader->m_end_idx) {
                    this->m_reader = nullptr;
                    return *this; // end();
                }
                if (m_reader->buf_exceed(m_reader->m_idx)) {
                    m_reader->read_blocks(m_reader->m_idx, m_reader->m_end_idx);
                    m_reader->m_begin_idx = m_reader->m_idx;
                }
                get_current_block();
                return *this;
            }

            bool operator!=(const iterator &rhs) const {
                return this->m_reader != rhs.m_reader;
            }

            const int reload() const {
                return m_reader->reload(m_reader->m_idx);
            }

            BlockReader *m_reader = nullptr;
        };

        iterator begin() {
            read_blocks(m_idx, m_end_idx);
            return iterator(this);
        }

        iterator end() {
            return iterator();
        }

        const CompressionFile *m_zfile = nullptr;
        off_t m_buf_offset = 0;
        size_t m_begin_idx = 0;
        size_t m_idx = 0;
        size_t m_end_idx = 0;
        off_t m_offset = 0;
        off_t m_end = 0;
        uint8_t m_verify = 0;
        uint32_t m_block_size = 0;
        unsigned char m_buf[MAX_READ_SIZE]; //{};
    };

    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {

        if (!valid) {
            LOG_ERROR_RETURN(EBADF, -1, "object invalid.");
        }
        if (m_ht.opt.block_size > MAX_READ_SIZE) {
            LOG_ERROR_RETURN(ENOMEM, -1, "block_size: ` > MAX_READ_SIZE (`)", m_ht.opt.block_size,
                             MAX_READ_SIZE);
        }
        if (offset + count > m_ht.raw_data_size) {
            LOG_WARN("the read range exceeds raw_file_size.(`>`)", count + offset,
                     m_ht.raw_data_size);
            count = m_ht.raw_data_size - offset;
        }
        if (count <= 0)
            return 0;
        assert(offset + count <= m_ht.raw_data_size);
        ssize_t readn = 0; // final will equal to count
        auto start_addr = buf;
        unsigned char raw[MAX_READ_SIZE];
        for (auto &block : BlockReader(this, offset, count)) {
            if (buf == nullptr) {
                //used for prefetch; no copy, no decompress;
                readn += block.cp_len;
                continue;
            }
            if (m_ht.opt.verify) {
                int retry = 2;
            again:
                auto c = crc32c((void *)block.buffer(), block.compressed_size);
                if (c != block.crc32_code()) {
                    if (retry--) {
                        int reload_res = block.reload();
                        LOG_ERROR(
                            "checksum failed {offset: `, length: `} (expected ` but got `), reload result: `",
                            block.m_reader->m_buf_offset, block.compressed_size, block.crc32_code(),
                            c, reload_res);
                        if (reload_res < 0) {
                            LOG_ERROR_RETURN(ECHECKSUM, -1,
                                             "checksum verification and reload failed");
                        }
                        goto again;
                    } else {
                        LOG_ERROR_RETURN(
                            ECHECKSUM, -1,
                            "checksum verification failed after retries {offset: `, length: `}",
                            block.m_reader->m_buf_offset, block.compressed_size);
                    }
                }
            }
            if (block.cp_len == m_ht.opt.block_size) {
                auto dret = m_compressor->decompress(block.buffer(), block.compressed_size,
                                                     (unsigned char *)buf, m_ht.opt.block_size);
                if (dret == -1)
                    return -1;
            } else {
                auto dret = m_compressor->decompress(block.buffer(), block.compressed_size, raw,
                                                     m_ht.opt.block_size);
                if (dret == -1)
                    return -1;
                memcpy(buf, raw + block.cp_begin, block.cp_len);
            }
            readn += block.cp_len;
            buf = (unsigned char *)buf + block.cp_len;
        }
        return readn;
    }
};

static int write_header_trailer(IFile *file, bool is_header, bool is_sealed, bool is_data_file,
                                CompressionFile::HeaderTrailer *pht, off_t offset = -1);

size_t compress_data(ICompressor *compressor, const unsigned char *buf, size_t count,
                     unsigned char *dest_buf, size_t dest_len, bool gen_crc) {

    size_t compressed_len = 0;
    auto ret = compressor->compress((const unsigned char *)buf, count, dest_buf, dest_len);
    if (ret <= 0) {
        LOG_ERRNO_RETURN(0, -1, "compress data failed.");
    }
    // LOG_DEBUG("compress buffer {offset: `, count: `} into ` bytes.", i, step, ret);
    compressed_len = ret;
    if (gen_crc) {
        auto crc32_code = crc32c(dest_buf, compressed_len);
        *((uint32_t *)&dest_buf[compressed_len]) = crc32_code;
        LOG_DEBUG("append ` bytes crc32_code: `", sizeof(uint32_t), crc32_code);
        compressed_len += sizeof(uint32_t);
    }
    LOG_DEBUG("compressed ` bytes into ` bytes.", count, compressed_len);
    return compressed_len;
}

class ZFileBuilder : public VirtualReadOnlyFile {
public:
    ZFileBuilder(IFile *file, const CompressArgs *args, bool ownership)
        : m_dest(file), m_opt(args->opt), m_ownership(ownership) {

        LOG_INFO("create stream compressing object. [ block size: `, type: `, enable_checksum: `]",
                 m_opt.block_size, m_opt.type, m_opt.verify);
    }

    int init(const CompressArgs *args) {
        m_compressor = create_compressor(args);
        if (m_compressor == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "create compressor failed.");
        }
        auto pht = new (m_ht)(CompressionFile::HeaderTrailer);
        pht->set_compress_option(m_opt);
        LOG_INFO("write header.");
        auto ret = write_header_trailer(m_dest, true, false, true, pht);
        if (ret < 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to write header");
        }
        moffset =
            CompressionFile::HeaderTrailer::SPACE + 0; // opt.dict_size;
                                                       // currently dictionary is not supported.
        m_buf_size = m_opt.block_size + BUF_SIZE;
        compressed_data = new unsigned char[m_buf_size];
        reserved_buf = new unsigned char[m_buf_size];
        return 0;
    }

    int write_buffer(const unsigned char *buf, size_t count) {
        auto compressed_len =
            compress_data(m_compressor, buf, count, compressed_data, m_buf_size, m_opt.verify);
        if (compressed_len <= 0) {
            LOG_ERRNO_RETURN(EIO, -1, "compress buffer failed.");
        }
        if (m_dest->write(compressed_data, compressed_len) != compressed_len) {
            LOG_ERRNO_RETURN(0, -1, "write compressed data failed.");
        }
        m_block_len.push_back(compressed_len);
        moffset += compressed_len;
        return 0;
    }

    int fini() {
        if (reserved_size) {
            LOG_INFO("compress reserved data.");
            if (write_buffer(reserved_buf, reserved_size) != 0)
                return -1;
        }
        uint64_t index_offset = moffset;
        uint64_t index_size = m_block_len.size();
        ssize_t index_bytes = index_size * sizeof(uint32_t);
        LOG_INFO("write index (offset: `, count: ` size: `)", index_offset, index_size,
                 index_bytes);
        if (m_dest->write(&m_block_len[0], index_bytes) != index_bytes) {
            LOG_ERRNO_RETURN(0, -1, "failed to write index.");
        }
        auto pht = (CompressionFile::HeaderTrailer *)m_ht;
        pht->index_offset = index_offset;
        pht->index_size = index_size;
        pht->raw_data_size = raw_data_size;
        LOG_INFO("write trailer.");
        auto ret = write_header_trailer(m_dest, false, true, true, pht);
        if (ret < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to write trailer");
        LOG_INFO("overwrite file header.");
        ret = write_header_trailer(m_dest, true, false, true, pht, 0);
        if (ret < 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to overwrite header");
        }
        return 0;
    }

    virtual int close() override {
        auto ret = fini();
        delete m_compressor;
        delete[] compressed_data;
        if (m_ownership) {
            m_dest->close();
        }
        return 0;
    }

    virtual ssize_t write(const void *buf, size_t count) override {
        LOG_DEBUG("generate zfile data(raw_data size: `)", count);
        raw_data_size += count;
        auto expected_ret = count;
        if (reserved_size != 0) {
            if (reserved_size + count < m_opt.block_size) {
                memcpy(reserved_buf + reserved_size, buf, count);
                reserved_size += count;
                return expected_ret; // save uncompressed buffer and return write count;
            }
            auto delta = m_opt.block_size - reserved_size;
            memcpy(reserved_buf + reserved_size, (unsigned char *)buf, delta);
            buf = (const void *)((unsigned char *)buf + delta);
            count -= delta;
            if (write_buffer(reserved_buf, reserved_size + delta) != 0) {
                LOG_ERRNO_RETURN(EIO, -1, "compress buffer failed.");
            }
            reserved_size = 0;
        }
        auto prev = moffset;
        for (off_t i = 0; i < count; i += m_opt.block_size) {
            if (i + m_opt.block_size > count) {
                memcpy(reserved_buf, (unsigned char *)buf + i, count - i);
                reserved_size = count - i;
                LOG_DEBUG("reserved data size: `", reserved_size);
                break;
            }
            auto ret = write_buffer((unsigned char *)buf + i, m_opt.block_size);
            if (ret != 0) {
                LOG_ERRNO_RETURN(EIO, -1, "compress buffer failed.");
            }
        }
        LOG_DEBUG("compressed ` bytes done. reserved: `", expected_ret, reserved_size);
        return expected_ret;
    }

    IFile *m_dest;
    off_t moffset = 0;
    size_t raw_data_size = 0;
    size_t m_buf_size = 0;
    CompressOptions m_opt;
    ICompressor *m_compressor = nullptr;
    bool m_ownership = false;
    std::vector<uint32_t> m_block_len;
    unsigned char *compressed_data = nullptr;
    unsigned char *reserved_buf = nullptr;
    size_t reserved_size = 0;
    char m_ht[CompressionFile::HeaderTrailer::SPACE]{};

    UNIMPLEMENTED_POINTER(IFileSystem *filesystem() override);
    UNIMPLEMENTED(int fstat(struct stat *buf) override);
};

// static std::unique_ptr<uint64_t[]>
bool load_jump_table(IFile *file, CompressionFile::HeaderTrailer *pheader_trailer,
                     CompressionFile::JumpTable &jump_table, bool trailer = true) {
    char buf[CompressionFile::HeaderTrailer::SPACE];
    auto ret = file->pread(buf, CompressionFile::HeaderTrailer::SPACE, 0);
    if (ret < (ssize_t)CompressionFile::HeaderTrailer::SPACE) {
        LOG_ERRNO_RETURN(0, false, "failed to read file header.");
    }

    auto pht = (CompressionFile::HeaderTrailer *)buf;
    if (!pht->verify_magic() || !pht->is_header()) {
        LOG_ERROR_RETURN(0, false, "header magic/type don't match");
    }

    struct stat stat;
    ret = file->fstat(&stat);
    if (ret < 0) {
        LOG_ERRNO_RETURN(0, false, "failed to stat file.");
    }
    uint64_t index_bytes = 0;
    if (!pht->is_header_overwrite()) {
        struct stat stat;
        ret = file->fstat(&stat);
        if (ret < 0) {
            LOG_ERRNO_RETURN(0, false, "failed to stat file.");
        }
        if (!pht->is_data_file()) {
            LOG_ERROR_RETURN(0, false, "uncognized file type");
        }

        auto trailer_offset = stat.st_size - CompressionFile::HeaderTrailer::SPACE;
        ret = file->pread(buf, CompressionFile::HeaderTrailer::SPACE, trailer_offset);
        if (ret < (ssize_t)CompressionFile::HeaderTrailer::SPACE)
            LOG_ERRNO_RETURN(0, false, "failed to read file trailer.");

        if (!pht->verify_magic() || !pht->is_trailer() || !pht->is_data_file() ||
            !pht->is_sealed()) {
            LOG_ERROR_RETURN(0, false,
                             "trailer magic, trailer type, file type or sealedness doesn't match");
        }

        index_bytes = pht->index_size * sizeof(uint32_t);
        LOG_INFO("trailer_offset: `, idx_offset: `, idx_bytes: `, dict_size: `, use_dict: `",
                 trailer_offset, pht->index_offset, index_bytes, pht->opt.dict_size,
                 pht->opt.use_dict);

        if (index_bytes > trailer_offset - pht->index_offset)
            LOG_ERROR_RETURN(0, false, "invalid index bytes or size. ");
    } else {
        index_bytes = pht->index_size * sizeof(uint32_t);
        LOG_INFO("read overwrite header. idx_offset: `, idx_bytes: `, dict_size: `, use_dict: `",
                 pht->index_offset, index_bytes, pht->opt.dict_size, pht->opt.use_dict);
    }
    auto ibuf = std::unique_ptr<uint32_t[]>(new uint32_t[pht->index_size]);
    LOG_DEBUG("index_offset: `", pht->index_offset);
    ret = file->pread((void *)(ibuf.get()), index_bytes, pht->index_offset);
    jump_table.build(ibuf.get(), pht->index_size,
                     CompressionFile::HeaderTrailer::SPACE + pht->opt.dict_size,
                     pht->opt.block_size);
    if (pheader_trailer)
        *pheader_trailer = *pht;
    return true;
}

IFile *zfile_open_ro(IFile *file, bool verify, bool ownership) {
    if (!file) {
        LOG_ERRNO_RETURN(EINVAL, nullptr, "invalid file ptr. (NULL)");
    }
    CompressionFile::HeaderTrailer ht;
    CompressionFile::JumpTable jump_table;
    int retry = 2;
again:
    if (!load_jump_table(file, &ht, jump_table, true)) {
        if (verify && retry--) {
            // verify means the source can be evicted. evict and retry
            auto res = file->fallocate(0, 0, -1);
            LOG_ERROR("failed load_jump_table, fallocate result: `", res);
            if (res < 0) {
                LOG_ERRNO_RETURN(EIO, nullptr,
                                 "failed to read index for file: `, fallocate failed, no retry",
                                 file);
            }
            goto again;
        }
        LOG_ERRNO_RETURN(EIO, nullptr, "failed to read index for file: `", file);
    }
    auto zfile = new CompressionFile(file, ownership);
    zfile->m_ht = ht;
    zfile->m_jump_table = std::move(jump_table);
    CompressArgs args(ht.opt);
    ht.opt.verify = ht.opt.verify && verify;
    LOG_DEBUG("compress type: `, bs: `, verify_checksum: `", ht.opt.type, ht.opt.block_size,
              ht.opt.verify);

    zfile->m_compressor.reset(create_compressor(&args));
    zfile->m_ownership = ownership;
    zfile->valid = true;
    return zfile;
}

static int write_header_trailer(IFile *file, bool is_header, bool is_sealed, bool is_data_file,
                                CompressionFile::HeaderTrailer *pht, off_t offset) {

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
    if (offset != -1)
        pht->set_header_overwrite();

    LOG_INFO("pht->opt.dict_size: `", pht->opt.dict_size);
    if (offset == -1) {
        return (int)file->write(pht, CompressionFile::HeaderTrailer::SPACE);
    }
    return (int)file->pwrite(pht, CompressionFile::HeaderTrailer::SPACE, offset);
}

int zfile_compress(IFile *file, IFile *as, const CompressArgs *args) {

    if (args == nullptr) {
        LOG_ERROR_RETURN(EINVAL, -1, "CompressArgs is null");
    }
    if (file == nullptr || as == nullptr) {
        LOG_ERROR_RETURN(EINVAL, -1, "file ptr is NULL (file: `, as: `)", file, as);
    }
    CompressOptions opt = args->opt;
    LOG_INFO("create compress file. [ block size: `, type: `, enable_checksum: `]", opt.block_size,
             opt.type, opt.verify);
    auto compressor = create_compressor(args);
    DEFER(delete compressor);
    if (compressor == nullptr)
        return -1;
    char buf[CompressionFile::HeaderTrailer::SPACE] = {};
    auto pht = new (buf) CompressionFile::HeaderTrailer;
    pht->set_compress_option(opt);

    LOG_INFO("write header.");
    auto ret = write_header_trailer(as, true, false, true, pht);
    if (ret < 0) {
        LOG_ERRNO_RETURN(0, -1, "failed to write header");
    }
    auto raw_data_size = file->lseek(0, SEEK_END);
    LOG_INFO("source data size: `", raw_data_size);
    auto block_size = opt.block_size;
    LOG_INFO("block size: `", block_size);
    auto buf_size = block_size + BUF_SIZE;
    bool crc32_verify = opt.verify;
    std::vector<uint32_t> block_len{};
    uint64_t moffset = CompressionFile::HeaderTrailer::SPACE + opt.dict_size;
    int nbatch = compressor->nbatch();
    LOG_DEBUG("nbatch: `, buffer need allocate: `", nbatch, nbatch * buf_size);
    auto raw_data = new unsigned char[nbatch * buf_size];
    DEFER(delete[] raw_data);
    auto compressed_data = new unsigned char[nbatch * buf_size];
    DEFER(delete[] compressed_data);
    std::vector<size_t> raw_chunk_len, compressed_len;
    compressed_len.resize(nbatch);
    raw_chunk_len.resize(nbatch);
    LOG_INFO("compress with start....");
    off_t i = 0;
    while (i < raw_data_size) {
        int n = 0;
        auto step = std::min((ssize_t)block_size * nbatch, (ssize_t)(raw_data_size - i));
        auto ret = file->pread(raw_data, step, i);
        if (ret < step) {
            LOG_ERRNO_RETURN(0, -1, "failed to read from source file. (readn: `)", ret);
        }
        i += step;
        while (step > 0) {
            if (step < block_size) {
                raw_chunk_len[n++] = step;
                break;
            }
            raw_chunk_len[n++] = block_size;
            step -= block_size;
        }
        ret = compressor->compress_batch(raw_data, &(raw_chunk_len[0]), compressed_data,
                                         n * buf_size, &(compressed_len[0]), n);
        if (ret != 0)
            return -1;
        for (off_t j = 0; j < n; j++) {
            ret = as->write(&compressed_data[j * buf_size], compressed_len[j]);
            if (ret < (ssize_t)compressed_len[j]) {
                LOG_ERRNO_RETURN(0, -1, "failed to write compressed data.");
            }
            if (crc32_verify) {
                auto crc32_code = crc32c(&compressed_data[j * buf_size], compressed_len[j]);
                LOG_DEBUG("append ` bytes crc32_code: {offset: `, count: `, crc32: `}",
                          sizeof(uint32_t), moffset, compressed_len[j], crc32_code);
                compressed_len[j] += sizeof(uint32_t);
                ret = as->write(&crc32_code, sizeof(uint32_t));
                if (ret < sizeof(uint32_t)) {
                    LOG_ERRNO_RETURN(0, -1, "failed to write crc32code, offset: `, crc32: `",
                                     moffset, crc32_code);
                }
            }
            block_len.push_back(compressed_len[j]);
            moffset += compressed_len[j];
        }
    }
    uint64_t index_offset = moffset;
    uint64_t index_size = block_len.size();
    ssize_t index_bytes = index_size * sizeof(uint32_t);
    LOG_INFO("write index (offset: `, count: ` size: `)", index_offset, index_size, index_bytes);
    if (as->write(&block_len[0], index_bytes) != index_bytes) {
        LOG_ERRNO_RETURN(0, -1, "failed to write index.");
    }
    pht->index_offset = index_offset;
    pht->index_size = index_size;
    pht->raw_data_size = raw_data_size;
    LOG_INFO("write trailer.");
    ret = write_header_trailer(as, false, true, true, pht);
    if (ret < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to write trailer");
    LOG_INFO("overwrite file header.");
    ret = write_header_trailer(as, true, false, true, pht, 0);
    if (ret < 0) {
        LOG_ERRNO_RETURN(0, -1, "failed to overwrite header");
    }
    return 0;
}

int zfile_decompress(IFile *src, IFile *dst) {
    auto file = (CompressionFile *)zfile_open_ro(src, /*verify = */ true);
    DEFER(delete file);
    if (file == nullptr) {
        LOG_ERROR_RETURN(0, -1, "failed to read file.");
    }
    struct stat _st;
    file->fstat(&_st);
    auto raw_data_size = _st.st_size;
    size_t block_size = file->m_ht.opt.block_size;

    auto raw_buf = std::unique_ptr<unsigned char[]>(new unsigned char[block_size]);
    for (off_t offset = 0; offset < raw_data_size; offset += block_size) {
        auto len = (ssize_t)std::min(block_size, (size_t)raw_data_size - offset);
        auto readn = file->pread(raw_buf.get(), len, offset);
        LOG_DEBUG("readn: `, crc32: `", readn, crc32c(raw_buf.get(), len));
        if (readn != len)
            return -1;
        if (dst->write(raw_buf.get(), readn) != readn) {
            LOG_ERRNO_RETURN(0, -1, "failed to write file into dst");
        }
    }
    return 0;
}

int is_zfile(IFile *file) {
    char buf[CompressionFile::HeaderTrailer::SPACE];
    auto ret = file->pread(buf, CompressionFile::HeaderTrailer::SPACE, 0);
    if (ret < (ssize_t)CompressionFile::HeaderTrailer::SPACE)
        LOG_ERRNO_RETURN(0, -1, "failed to read file header.");
    auto pht = (CompressionFile::HeaderTrailer *)buf;
    if (!pht->verify_magic() || !pht->is_header()) {
        LOG_DEBUG("file: ` is not a zfile object", file);
        return 0;
    }
    LOG_DEBUG("file: ` is a zfile object", file);
    return 1;
}

IFile *new_zfile_builder(IFile *file, const CompressArgs *args, bool ownership) {
    auto r = new ZFileBuilder(file, args, ownership);
    if (r->init(args) != 0) {
        delete r;
        LOG_ERRNO_RETURN(0, nullptr, "init zfileStreamWriter failed.");
    }
    return r;
}
} // namespace ZFile
