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

#include "fsmeta_file.h"
#include <photon/common/alog.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <linux/falloc.h>

constexpr size_t FsMetaFile::BLOCK_SIZE;

bool FsMetaFile::dirty(size_t block) const {
    return m_dirty[block / 64] & (uint64_t(1) << (block % 64));
}

void FsMetaFile::mark(size_t block) {
    m_dirty[block / 64] |= uint64_t(1) << (block % 64);
}

void FsMetaFile::clear(size_t block) {
    m_dirty[block / 64] &= ~(uint64_t(1) << (block % 64));
}

bool FsMetaFile::valid_range(off_t offset, size_t count) const {
    return offset >= 0 && (uint64_t)offset <= m_size && count <= m_size - offset;
}

ssize_t FsMetaFile::pread(void *buf, size_t count, off_t offset) {
    if (!valid_range(offset, count)) {
        LOG_ERROR_RETURN(EINVAL, -1, "invalid fs metadata read range");
    }
    size_t done = 0;
    while (done < count) {
        auto pos = offset + done;
        auto block = pos / BLOCK_SIZE;
        auto in_block = pos % BLOCK_SIZE;
        auto source = dirty(block) ? m_sparse : m_base;
        size_t n = std::min(count - done, BLOCK_SIZE - in_block);
        if (in_block != 0 || n < BLOCK_SIZE) {
            alignas(BLOCK_SIZE) char data[BLOCK_SIZE];
            auto block_bytes = std::min(BLOCK_SIZE, m_size - block * BLOCK_SIZE);
            if (source->pread(data, block_bytes, block * BLOCK_SIZE) != (ssize_t)block_bytes) {
                LOG_ERRNO_RETURN(EIO, -1, "failed to read fs metadata block");
            }
            memcpy((char *)buf + done, data + in_block, n);
        } else {
            while (n + BLOCK_SIZE <= count - done &&
                   dirty(block + n / BLOCK_SIZE) == dirty(block)) {
                n += BLOCK_SIZE;
            }
            if (source->pread((char *)buf + done, n, pos) != (ssize_t)n) {
                LOG_ERRNO_RETURN(EIO, -1, "failed to read fs metadata");
            }
        }
        done += n;
    }
    return done;
}

ssize_t FsMetaFile::pwrite(const void *buf, size_t count, off_t offset) {
    if (!valid_range(offset, count)) {
        LOG_ERROR_RETURN(EINVAL, -1, "invalid fs metadata write range");
    }
    size_t done = 0;
    while (done < count) {
        auto pos = offset + done;
        auto block = pos / BLOCK_SIZE;
        auto in_block = pos % BLOCK_SIZE;
        size_t n = std::min(count - done, BLOCK_SIZE - in_block);
        if (in_block != 0 || n < BLOCK_SIZE) {
            alignas(BLOCK_SIZE) char data[BLOCK_SIZE] = {0};
            auto source = dirty(block) ? m_sparse : m_base;
            auto block_bytes = std::min(BLOCK_SIZE, m_size - block * BLOCK_SIZE);
            if (source->pread(data, block_bytes, block * BLOCK_SIZE) != (ssize_t)block_bytes) {
                m_write_error = EIO;
                LOG_ERRNO_RETURN(EIO, -1, "failed to read partial fs metadata block");
            }
            memcpy(data + in_block, (const char *)buf + done, n);
            if (m_sparse->pwrite(data, BLOCK_SIZE, block * BLOCK_SIZE) != (ssize_t)BLOCK_SIZE) {
                m_write_error = EIO;
                LOG_ERRNO_RETURN(EIO, -1, "failed to stage fs metadata block");
            }
            mark(block);
        } else {
            n = (count - done) / BLOCK_SIZE * BLOCK_SIZE;
            if (m_sparse->pwrite((const char *)buf + done, n, pos) != (ssize_t)n) {
                m_write_error = EIO;
                LOG_ERRNO_RETURN(EIO, -1, "failed to stage fs metadata");
            }
            for (size_t i = 0; i < n / BLOCK_SIZE; i++) {
                mark(block + i);
            }
        }
        done += n;
    }
    return done;
}

int FsMetaFile::fallocate(int mode, off_t offset, off_t len) {
    if (len <= 0 || !valid_range(offset, len)) {
        LOG_ERROR_RETURN(EINVAL, -1, "invalid fs metadata fallocate range");
    }
    if (mode != (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE) &&
        mode != FALLOC_FL_ZERO_RANGE && mode != (FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE)) {
        LOG_ERROR_RETURN(ENOTSUP, -1, "unsupported fs metadata fallocate mode");
    }
    if (m_base->fallocate(mode, offset, len) != 0) {
        return -1;
    }
    auto end = offset + len;
    for (auto pos = offset; pos < end;) {
        auto block = pos / BLOCK_SIZE;
        auto in_block = pos % BLOCK_SIZE;
        size_t n = std::min<size_t>(end - pos, BLOCK_SIZE - in_block);
        if (dirty(block)) {
            if (in_block == 0 && n == BLOCK_SIZE) {
                clear(block);
            } else {
                alignas(BLOCK_SIZE) char data[BLOCK_SIZE];
                if (m_sparse->pread(data, BLOCK_SIZE, block * BLOCK_SIZE) != (ssize_t)BLOCK_SIZE) {
                    m_write_error = EIO;
                    LOG_ERRNO_RETURN(EIO, -1, "failed to read fs metadata before zeroing");
                }
                memset(data + in_block, 0, n);
                if (m_sparse->pwrite(data, BLOCK_SIZE, block * BLOCK_SIZE) != (ssize_t)BLOCK_SIZE) {
                    m_write_error = EIO;
                    LOG_ERRNO_RETURN(EIO, -1, "failed to zero staged fs metadata");
                }
            }
        }
        pos += n;
    }
    return 0;
}

void FsMetaFile::discard(off_t offset, size_t count) {
    if (count == 0) {
        return;
    }
    auto end = (offset + count + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (size_t block = offset / BLOCK_SIZE; block < end; block++) {
        clear(block);
    }
}

int FsMetaFile::commit() {
    // Ext4's close path ignores I/O errors, including short writes to already-dirty blocks.
    if (m_write_error != 0) {
        LOG_ERROR_RETURN(m_write_error, -1, "fs metadata staging failed");
    }
    const size_t buffer_size = 1 << 20;
    void *buf = nullptr;
    if (posix_memalign(&buf, BLOCK_SIZE, buffer_size) != 0) {
        LOG_ERROR_RETURN(ENOMEM, -1, "failed to allocate fs metadata buffer");
    }
    DEFER(free(buf));
    size_t bytes = 0;
    for (size_t word = 0; word < m_dirty.size(); word++) {
        while (m_dirty[word] != 0) {
            size_t first = word * 64 + __builtin_ctzll(m_dirty[word]);
            size_t blocks = 1;
            while (blocks < buffer_size / BLOCK_SIZE && (first + blocks) * BLOCK_SIZE < m_size &&
                   dirty(first + blocks)) {
                blocks++;
            }
            auto offset = first * BLOCK_SIZE;
            auto count = std::min(blocks * BLOCK_SIZE, m_size - offset);
            if (m_sparse->pread(buf, count, offset) != (ssize_t)count) {
                LOG_ERRNO_RETURN(EIO, -1, "failed to read staged fs metadata");
            }
            if (m_base->pwrite(buf, count, offset) != (ssize_t)count) {
                LOG_ERRNO_RETURN(EIO, -1, "failed to append fs metadata");
            }
            for (size_t i = 0; i < blocks; i++) {
                clear(first + i);
            }
            bytes += count;
        }
    }
    LOG_INFO("appended ` bytes of compacted fs metadata", bytes);
    return 0;
}
