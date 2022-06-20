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
#include <vector>
#include "lz4/lz4-qat.h"
extern "C" {
#include <pci/pci.h>
}

using namespace std;

namespace ZFile {

#define QAT_VENDOR_ID 0x8086
#define QAT_DEVICE_ID 0x4940

class Compressor_lz4 : public ICompressor {
public:
    uint32_t max_dst_size = 0;
    uint32_t src_blk_size = 0;
    bool qat_enable = false;

    LZ4_qat_param *pQat = nullptr;

    vector<unsigned char *> raw_data;
    vector<unsigned char *> compressed_data;

    const int DEFAULT_N_BATCH = 256;

    ~Compressor_lz4() {
        if (pQat) {
            qat_uninit(pQat);
            delete pQat;
        }
    }

    bool check_qat() {

#ifdef ENABLE_QAT
        struct pci_access *pacc;
        struct pci_dev *dev;
        pacc = pci_alloc();
        pci_init(pacc);
        pci_scan_bus(pacc);
        for (dev = pacc->devices; dev; dev = dev->next) {
            pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES);
            if (dev->vendor_id == QAT_VENDOR_ID && dev->device_id == QAT_DEVICE_ID) {
                pci_cleanup(pacc);
                return true;
            }
        }
        pci_cleanup(pacc);

        return false;
#endif
        return false;
    }

    int init(const CompressArgs *args) {
        auto opt = &args->opt;
        if (opt == nullptr) {
            LOG_ERROR_RETURN(EINVAL, -1, "CompressOptions* is nullptr.");
        }
        if (opt->type != CompressOptions::LZ4) {
            LOG_ERROR_RETURN(EINVAL, -1,
                             "Compression type invalid. (expected: CompressionOptions::LZ4)");
        }
        src_blk_size = opt->block_size;
        max_dst_size = LZ4_compressBound(src_blk_size);
        if (check_qat()) {
            pQat = new LZ4_qat_param();
            qat_init(pQat);
            qat_enable = true;
        }
        LOG_INFO("create batch buffer, size: `", nbatch());
        raw_data.resize(nbatch());
        compressed_data.resize(nbatch());
        return 0;
    }

    int compress(const unsigned char *src, size_t src_len, unsigned char *dst,
                 size_t dst_len) override {

        size_t dst_chunk_len;
        if (compress_batch(src, &src_len, dst, dst_len, &dst_chunk_len, 1) != 0) {
            LOG_ERRNO_RETURN(0, -1, "compress data failed.");
        }
        return dst_chunk_len; // return decompressed size for single block.
    }

    int nbatch() override {
        // return DEFAULT_N_BATCH;
        return (qat_enable ? DEFAULT_N_BATCH : 1);
    }

    int compress_batch(const unsigned char *src, size_t *src_chunk_len, unsigned char *dst,
                       size_t dst_buffer_capacity, size_t *dst_chunk_len, size_t n) override {

        if (dst_buffer_capacity / n < max_dst_size) {
            LOG_ERROR_RETURN(ENOBUFS, -1, "dst_len should be greater than `", max_dst_size - 1);
        }
        off_t src_offset = 0, dst_offset = 0;
        int ret = 0;
        for (ssize_t i = 0; i < n; i++) {
            raw_data[i] = ((unsigned char *)src + src_offset);
            compressed_data[i] = ((unsigned char *)dst + dst_offset);
            src_offset += src_chunk_len[i];
            dst_offset += dst_buffer_capacity / n;
        }
#ifdef ENABLE_QAT
        if (qat_enable) {
            ret = LZ4_compress_qat(pQat, &raw_data[0], src_chunk_len, &compressed_data[0], dst_chunk_len, n);
            if (ret < 0) {
                LOG_ERROR_RETURN(EFAULT, -1, "LZ4 compress data failed. (retcode: `).", ret);
            }
            return ret;
        }
#endif
        for (ssize_t i = 0; i < n; i++) {
            ret = LZ4_compress_default((const char *)raw_data[i], (char *)compressed_data[i],
                                       src_chunk_len[i], dst_buffer_capacity / n);
           
            dst_chunk_len[i] = ret;
            if (ret < 0) {
                LOG_ERROR_RETURN(EFAULT, -1, "LZ4 compress data failed. (retcode: `).", ret);
            }
            if (ret == 0) {
                LOG_ERROR_RETURN(EFAULT, -1,
                    "Compression worked, but was stopped because the *dst couldn't hold all the information.");
            }
        }
        return 0;
    }

    int decompress(const unsigned char *src, size_t src_len, unsigned char *dst,
                   size_t dst_len) override {

        /*
        The code prepared for QAT.

            if (dst_len < src_blk_size) {
                LOG_ERROR_RETURN(0, -1, "dst_len (`) should be greater than compressed block size `",
                                dst_len, src_blk_size);
            }
            auto ret = LZ4_decompress_qat(pQat, (const char *)src, (char *)dst, src_len, dst_len);
            if (ret < 0) {
                LOG_ERROR_RETURN(EFAULT, -1, "LZ4 decompress data failed. (retcode: `)", ret);
            }
            if (ret == 0) {
                LOG_ERROR_RETURN(EFAULT, -1, "LZ4 decompress returns 0. THIS SHOULD BE NEVER HAPPEN!");
            }
            LOG_DEBUG("decompressed ` bytes back into ` bytes.", src_len, ret);
            return ret;
        */
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
