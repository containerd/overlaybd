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

#include "compressor.h"
#include "lz4/lz4.h"
#include <photon/common/alog.h>
#include <memory>

namespace ZFile {

class Compressor_lz4 : public ICompressor {
public:
    uint32_t max_dst_size = 0;
    uint32_t src_blk_size = 0;

    int init(const CompressArgs *args) {
        auto opt = &args->opt;
        if (opt == nullptr) {
            LOG_ERROR_RETURN(EINVAL, -1, "CompressOptions* is nullptr.");
        };
        if (opt->type != CompressOptions::LZ4) {
            LOG_ERROR_RETURN(EINVAL, -1,
                             "Compression type invalid. (expected: CompressionOptions::LZ4)");
        }
        src_blk_size = opt->block_size;
        max_dst_size = LZ4_compressBound(src_blk_size);
        return 0;
    }

    int compress(const unsigned char *src, size_t src_len, unsigned char *dst,
                 size_t dst_len) override {
        if (dst_len < max_dst_size) {
            LOG_ERROR_RETURN(ENOBUFS, -1, "dst_len should be greater than `", max_dst_size - 1);
        }

        auto ret = LZ4_compress_default((const char *)src, (char *)dst, src_len, dst_len);
        if (ret < 0) {
            LOG_ERROR_RETURN(EFAULT, -1, "LZ4 compress data failed. (retcode: `).", ret);
        }
        if (ret == 0) {
            LOG_ERROR_RETURN(
                EFAULT, -1,
                "Compression worked, but was stopped because the *dst couldn't hold all the information.");
        }
        return ret;
    }

    int decompress(const unsigned char *src, size_t src_len, unsigned char *dst,
                   size_t dst_len) override {
        if (dst_len < src_blk_size) {
            LOG_ERROR_RETURN(0, -1, "dst_len (`) should be greater than compressed block size `",
                             dst_len, src_blk_size);
        }
        auto ret = LZ4_decompress_safe((const char *)src, (char *)dst, src_len, dst_len);
        if (ret < 0) {
            LOG_ERROR_RETURN(EFAULT, -1, "LZ4 decompress data failed. (retcode: `)", ret);
        }
        if (ret == 0) {
            LOG_ERROR_RETURN(EFAULT, -1, "LZ4 decompress returns 0. THIS SHOULD BE NEVER HAPPEN!");
        }
        LOG_DEBUG("decompressed ` bytes back into ` bytes.", src_len, ret);
        return ret;
    }
};

ICompressor *create_compressor(const CompressArgs *args) {
    ICompressor *rst = nullptr;
    int init_flg = 0;
    const CompressOptions &opt = args->opt;
    switch (opt.type) {

    case CompressOptions::LZ4:
        rst = new Compressor_lz4;
        if (rst != nullptr) {
            init_flg = ((Compressor_lz4 *)rst)->init(args);
        }
        break;

    default:
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid CompressionOptions.");
    }
    if (init_flg != 0) {
        delete rst;
        return nullptr;
    }
    return rst;
}

}; // namespace ZFile
