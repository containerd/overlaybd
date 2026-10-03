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
#pragma once

#include "compressor.h"
#include "lz4/lz4.h"
#include <zstd.h>
#include <cerrno>
#include <climits>
#include <vector>

namespace ZFile {

// Whole-index codecs use CPU routines: data-block/QAT buffer limits do not apply.
// Return 1 for compressed output, 0 to keep the original, or -1 on codec error.
inline int compress_index_buffer(const void *src, size_t size, uint8_t algo,
                                 std::vector<unsigned char> &dst) {
    dst.clear();
    if (algo != CompressOptions::LZ4 && algo != CompressOptions::ZSTD) {
        errno = EINVAL;
        return -1;
    }
    // A raw index remains valid even when a single codec call cannot represent it.
    if (size == 0 || size > INT_MAX ||
        (algo == CompressOptions::LZ4 && size > LZ4_MAX_INPUT_SIZE))
        return 0;
    size_t bound = algo == CompressOptions::LZ4
                       ? (size_t)LZ4_compressBound((int)size) : ZSTD_compressBound(size);
    if (bound == 0 || bound > INT_MAX)
        return 0;
    dst.resize(bound);
    size_t written;
    if (algo == CompressOptions::LZ4) {
        int ret = LZ4_compress_default((const char *)src, (char *)dst.data(),
                                       (int)size, (int)bound);
        if (ret <= 0) {
            errno = EIO;
            return -1;
        }
        written = ret;
    } else {
        // Match Compressor_zstd's current data compression level.
        written = ZSTD_compress(dst.data(), bound, src, size, 3);
        if (ZSTD_isError(written)) {
            errno = EIO;
            return -1;
        }
    }
    if (written >= size) {
        dst.clear();
        return 0;
    }
    dst.resize(written);
    return 1;
}

// A successful decode must restore exactly the expected index byte count.
inline int decompress_index_buffer(const void *src, size_t size, void *dst,
                                   size_t expected, uint8_t algo) {
    if (size == 0 || expected == 0 || size > INT_MAX || expected > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    size_t written;
    if (algo == CompressOptions::LZ4) {
        if (expected > LZ4_MAX_INPUT_SIZE) {
            errno = EINVAL;
            return -1;
        }
        int ret = LZ4_decompress_safe((const char *)src, (char *)dst,
                                     (int)size, (int)expected);
        if (ret < 0) {
            errno = EIO;
            return -1;
        }
        written = ret;
    } else if (algo == CompressOptions::ZSTD) {
        written = ZSTD_decompress(dst, expected, src, size);
        if (ZSTD_isError(written)) {
            errno = EIO;
            return -1;
        }
    } else {
        errno = EINVAL;
        return -1;
    }
    if (written != expected) {
        errno = EIO;
        return -1;
    }
    return 0;
}

} // namespace ZFile
