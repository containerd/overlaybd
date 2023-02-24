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
#ifndef __COMPRESSOR_H__
#define __COMPRESSOR_H__

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include "lz4/lz4.h"

namespace photon {
    namespace fs {
        class IFile;
    }
}

namespace ZFile {

/* CompressOption will write into file */
class CompressOptions {
public:
    const static uint8_t MINI_LZO = 0;
    const static uint8_t LZ4 = 1;
    const static uint8_t ZSTD = 2;
    const static uint32_t DEFAULT_BLOCK_SIZE = 4096; // 8192;//32768;

    uint32_t block_size = DEFAULT_BLOCK_SIZE;
    uint8_t type = LZ4; // algorithm
    uint8_t level = 0;  // compress level
    uint8_t use_dict = 0;
    uint8_t __padding_0 = 0;
    uint32_t args = 0; // reserve;
    uint32_t dict_size = 0;
    uint8_t verify = 0;
    uint8_t __padding_1[7] = {0};

    CompressOptions(uint8_t type = LZ4, uint32_t block_size = DEFAULT_BLOCK_SIZE,
                    uint8_t verify = 0)
        : block_size(block_size), type(type), verify(verify) {
    }
};
static_assert(sizeof(CompressOptions) == 24, "sizeof(CompressOptions) != 24");

class CompressArgs {
public:
    photon::fs::IFile *fdict = nullptr;
    std::unique_ptr<unsigned char[]> dict_buf = nullptr;
    CompressOptions opt;

    CompressArgs(const CompressOptions &opt, photon::fs::IFile *dict = nullptr,
                 unsigned char *dict_buf = nullptr)
        : fdict(dict), dict_buf(dict_buf), opt(opt) {
        if (fdict || dict_buf) {
            this->opt.use_dict = 1;
        }
    };
};

class ICompressor {
public:
    virtual ~ICompressor(){};
    /*
        return compressed buffer size.
        return -1 when error occurred.
    */
    virtual int compress(const unsigned char *src, size_t src_len, unsigned char *dst,
                         size_t dst_len) = 0;
    /* 
        return the number of batches in QAT compressing...
    */
    virtual int nbatch() = 0;
    virtual int compress_batch(const unsigned char *src, size_t *src_chunk_len, unsigned char *dst,
                        size_t dst_buffer_capacity, size_t *dst_chunk_len /* save result chunk length */, 
                        size_t nchunk) = 0;
    /*
        return decompressed buffer size.
        return -1 when error occurred.
    */
    virtual int decompress(const unsigned char *src, size_t src_len, unsigned char *dst,
                           size_t dst_len) = 0;

    virtual int decompress_batch(const unsigned char *src, size_t *src_chunk_len, unsigned char *dst,
                        size_t dst_buffer_capacity, size_t *dst_chunk_len /* save result chunk length */, 
                        size_t nchunk) = 0;
};

extern "C" ICompressor *create_compressor(const CompressArgs *args);
} // namespace ZFile

#endif