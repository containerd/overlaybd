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
#include <cstddef>
#include <photon/common/alog.h>
#include <memory>
#include <vector>
#include <zstd.h>
#include <sys/fcntl.h>
#include "photon/fs/filesystem.h"

#ifdef ENABLE_QAT
#include <qatzip.h>
#include <condition_variable>
#include <mutex>
#include <optional>
extern "C" {
#include <pci/pci.h>
}
#endif

using namespace std;

namespace ZFile {

#define QAT_VENDOR_ID 0x8086
#define QAT_DEVICE_ID 0x4940

class BaseCompressor : public ICompressor {
public:
    uint32_t max_dst_size = 0;
    uint32_t src_blk_size = 0;
    // vector<unsigned char *> raw_data;
    vector<unsigned char *> compressed_data;
    vector<unsigned char *> uncompressed_data;

    const int DEFAULT_N_BATCH = 1;

    virtual int init(const CompressArgs *args) {
        auto opt = &args->opt;
        if (opt == nullptr) {
            LOG_ERROR_RETURN(EINVAL, -1, "CompressOptions* is nullptr.");
        }

        src_blk_size = opt->block_size;
        LOG_DEBUG("create batch buffer, size: `", nbatch());
        // raw_data.resize(nbatch());
        compressed_data.resize(nbatch());
        uncompressed_data.resize(nbatch());
        return 0;
    }

    virtual int nbatch() override {
        return 1;
    }

    virtual int do_compress(size_t *src_chunk_len /* uncompressed length per block */,
                            size_t *dst_chunk_len, size_t dst_buffer_capacity, size_t nblock) = 0;

    virtual int do_decompress(size_t *src_chunk_len,
                              /* compressed length per block */ size_t *dst_chunk_len,
                              size_t dst_buffer_capacity, size_t nblock) = 0;

    virtual int compress(const unsigned char *src, size_t src_len, unsigned char *dst,
                         size_t dst_len) override {

        size_t dst_chunk_len;
        if (compress_batch(src, &src_len, dst, dst_len, &dst_chunk_len, 1) != 0) {
            LOG_ERRNO_RETURN(0, -1, "compress data failed.");
        }
        return dst_chunk_len; // return decompressed size for single block.
    }

    int compress_batch(const unsigned char *src, size_t *src_chunk_len, unsigned char *dst,
                       size_t dst_buffer_capacity, size_t *dst_chunk_len, size_t n) override {

        if (dst_buffer_capacity / n < max_dst_size) {
            LOG_ERROR_RETURN(ENOBUFS, -1, "dst_len should be greater than `", max_dst_size - 1);
        }
        off_t src_offset = 0, dst_offset = 0;
        for (size_t i = 0; i < n; i++) {
            uncompressed_data[i] = ((unsigned char *)src + src_offset);
            compressed_data[i] = ((unsigned char *)dst + dst_offset);
            src_offset += src_chunk_len[i];
            dst_offset += dst_buffer_capacity / n;
        }
        return do_compress(src_chunk_len, dst_chunk_len, dst_buffer_capacity, n);
    }

    virtual int decompress(const unsigned char *src, size_t src_len, unsigned char *dst,
                           size_t dst_len) override {

        size_t dst_chunk_len;
        if (decompress_batch(src, &src_len, dst, dst_len, &dst_chunk_len, 1) != 0) {
            LOG_ERRNO_RETURN(0, -1, "compress data failed.");
        }
        return dst_chunk_len; // return decompressed size for single block.
    }

    int decompress_batch(const unsigned char *src, size_t *src_chunk_len, unsigned char *dst,
                         size_t dst_buffer_capacity, size_t *dst_chunk_len, size_t n) override {

        if (dst_buffer_capacity / n < src_blk_size) {
            LOG_ERROR_RETURN(ENOBUFS, -1,
                             "dst_len (`) should be greater than compressed block size `",
                             dst_buffer_capacity / n, src_blk_size);
        }
        off_t src_offset = 0, dst_offset = 0;
        for (size_t i = 0; i < n; i++) {
            compressed_data[i] = ((unsigned char *)src + src_offset);
            uncompressed_data[i] = ((unsigned char *)dst + dst_offset);
            src_offset += src_chunk_len[i];
            dst_offset += dst_buffer_capacity / n;
        }

        return do_decompress(src_chunk_len, dst_chunk_len, dst_buffer_capacity, n);
    }
};

#ifdef ENABLE_QAT
struct QzSessionDeleter {
    void operator()(struct QzSession_S *session);
};

void QzSessionDeleter::operator()(struct QzSession_S *session) {
    qzTeardownSession(session);
    delete session;
}
#endif

class LZ4Compressor : public BaseCompressor {
public:
#ifdef ENABLE_QAT
    using session_ptr = std::unique_ptr<struct QzSession_S, QzSessionDeleter>;
#endif
    bool qat_enable = false;
    ~LZ4Compressor() {
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

    virtual int init(const CompressArgs *args) override {

        if (BaseCompressor::init(args) != 0) {
            LOG_ERROR_RETURN(EINVAL, -1, "BaseCompressor init failed");
        }
        auto opt = &args->opt;
        if (opt->algo != CompressOptions::LZ4) {
            LOG_ERROR_RETURN(EINVAL, -1,
                             "Compression type invalid. (expected: CompressionOptions::LZ4)");
        }
        max_dst_size = LZ4_compressBound(src_blk_size);
#ifdef ENABLE_QAT
        if (check_qat()) {
            qat_enable = true;
        }
#endif
        return 0;
    }

    int nbatch() override {
        // return DEFAULT_N_BATCH;
        return (qat_enable ? DEFAULT_N_BATCH : 1);
    }

    int do_compress(size_t *src_chunk_len, size_t *dst_chunk_len, size_t dst_buffer_capacity,
                    size_t nblock) override;

    int do_decompress(size_t *src_chunk_len, size_t *dst_chunk_len, size_t dst_buffer_capacity,
                      size_t n) override;
#ifdef ENABLE_QAT
private:
    session_ptr get_session();
    friend struct cached_session_t;
    std::vector<session_ptr> sessions;
    std::mutex mutex;
#endif
};

#ifdef ENABLE_QAT
static bool setup_session(LZ4Compressor::session_ptr &session) {
    int rc;
    rc = qzInit(session.get(), QZ_SW_BACKUP_DEFAULT);
    if (rc != QZ_OK && rc != QZ_DUPLICATE)
        return false;
    QzSessionParamsLZ4_T params;
    rc = qzGetDefaultsLZ4(&params);
    if (rc != QZ_OK)
        return false;
    // params.data_fmt = QZ_LZ4;
    params.common_params.comp_algorithm = QZ_LZ4;
    params.common_params.comp_lvl = 1;
    params.common_params.direction = QZ_DIR_BOTH;
    rc = qzSetupSessionLZ4(session.get(), &params);
    if (rc != QZ_OK)
        return false;

    return true;
}

// put the session back to the session pool in a RAII manner
struct cached_session_t {
    cached_session_t(const cached_session_t &);

    cached_session_t(LZ4Compressor *accel, LZ4Compressor::session_ptr &&sess)
        : accel{accel}, session{std::move(sess)} {
    }

    ~cached_session_t() {
        std::lock_guard<std::mutex> lock{accel->mutex};
        // if the cache size is still under its upper bound, the current session is put into
        // accel->sessions. otherwise it's released right
        uint64_t sessions_num = 256;
        // g_ceph_context->_conf.get_val<uint64_t>("qat_compressor_session_max_number");
        if (accel->sessions.size() < sessions_num) {
            accel->sessions.push_back(std::move(session));
        }
    }

    struct QzSession_S *get() {
        assert(static_cast<bool>(session));
        return session.get();
    }

    LZ4Compressor *accel;
    LZ4Compressor::session_ptr session;
};

LZ4Compressor::session_ptr LZ4Compressor::get_session() {
    {
        std::lock_guard<std::mutex> lock{mutex};
        if (!sessions.empty()) {
            auto session = std::move(sessions.back());
            sessions.pop_back();
            return session;
        }
    }

    // If there are no available session to use, we try allocate a new
    // session.
    session_ptr session(new struct QzSession_S());
    memset(session.get(), 0, sizeof(struct QzSession_S));
    if (setup_session(session)) {
        return session;
    } else {
        return nullptr;
    }
}
#endif

int LZ4Compressor::do_compress(size_t *src_chunk_len, size_t *dst_chunk_len,
                               size_t dst_buffer_capacity, size_t nblock) {

    int ret = 0;
#ifdef ENABLE_QAT
    if (qat_enable) {
        auto s = get_session(); // get a session from the pool
        if (!s) {
            LOG_INFO("get_session failed!");
            return -1; // session initialization failed
        }
        auto session =
            cached_session_t{this, std::move(s)}; // returns to the session pool on destruction

        for (size_t i = 0; i < nblock; i++) {
            unsigned int len = src_chunk_len[i];
            unsigned int out_len = qzMaxCompressedLength(len, session.get());
            int rc = qzCompress(session.get(), uncompressed_data[i], (&len), compressed_data[i],
                                (&out_len), 1);
            dst_chunk_len[i] = out_len;
            if (rc != QZ_OK) {
                LOG_INFO("qzCompress failed!, rc: `", rc);
                return -1;
            }
        }
        return 0;
    }
#endif
    for (size_t i = 0; i < nblock; i++) {
        ret = LZ4_compress_default((const char *)uncompressed_data[i], (char *)compressed_data[i],
                                   src_chunk_len[i], dst_buffer_capacity / nblock);

        dst_chunk_len[i] = ret;
        if (ret < 0) {
            LOG_ERROR_RETURN(EFAULT, -1, "LZ4 compress data failed. (retcode: `).", ret);
        }
        if (ret == 0) {
            LOG_ERROR_RETURN(
                EFAULT, -1,
                "Compression worked, but was stopped because the *dst couldn't hold all the information.");
        }
    }
    return 0;
}
int LZ4Compressor::do_decompress(size_t *src_chunk_len, size_t *dst_chunk_len,
                                 size_t dst_buffer_capacity, size_t n) {

    int ret = 0;
#ifdef ENABLE_QAT
    if (qat_enable) {
        auto s = get_session(); // get a session from the pool
        if (!s) {
            return -1; // session initialization failed
        }
        auto session =
            cached_session_t{this, std::move(s)}; // returns to the session pool on destruction

        for (size_t i = 0; i < n; i++) {
            int rc = qzDecompress(session.get(), compressed_data[i],
                                  reinterpret_cast<unsigned int *>(&src_chunk_len[i]),
                                  uncompressed_data[i],
                                  reinterpret_cast<unsigned int *>(&dst_chunk_len[i]));
            if (rc != QZ_OK)
                return -1;
        }
        return 0;
    }
#endif
    for (size_t i = 0; i < n; i++) {
        ret = LZ4_decompress_safe((const char *)compressed_data[i], (char *)uncompressed_data[i],
                                  src_chunk_len[i], dst_buffer_capacity / n);

        dst_chunk_len[i] = ret;
        if (ret < 0) {
            LOG_ERROR_RETURN(EFAULT, -1, "LZ4 decompress data failed. (retcode: `).", ret);
        }
        if (ret == 0) {
            LOG_ERROR_RETURN(EFAULT, -1, "LZ4 decompress returns 0. THIS SHOULD BE NEVER HAPPEN!");
        }
    }
    return 0;
}

class Compressor_zstd : public BaseCompressor {
public:
    static const int cLevel = 3;

    ~Compressor_zstd() {
    }

    virtual int init(const CompressArgs *args) override {
        if (BaseCompressor::init(args) != 0) {
            LOG_ERROR_RETURN(EINVAL, -1, "BaseCompressor init failed");
        }
        const CompressOptions *opt = &args->opt;
        if (opt->algo != CompressOptions::ZSTD) {
            LOG_ERROR_RETURN(EINVAL, -1,
                             "Compression type invalid.(expected: CompressionOptions::ZSTD)");
        }
        max_dst_size = ZSTD_compressBound(src_blk_size);
        return 0;
    }

    virtual int compress(const unsigned char *src, size_t src_len, unsigned char *dst,
                         size_t dst_len) override {
        if (dst_len < max_dst_size) {
            LOG_ERROR_RETURN(ENOBUFS, -1, "dst_len should be greater than `", max_dst_size - 1);
        }
        size_t cSize = ZSTD_compress(dst, dst_len, src, src_len, cLevel);
        if (ZSTD_isError(cSize)) {
            LOG_ERROR_RETURN(0, -1, "compress error: `", ZSTD_getErrorName(cSize));
        }
        return cSize;
    }

    virtual int do_compress(size_t *src_chunk_len /* uncompressed length per block */,
                            size_t *dst_chunk_len, size_t dst_buffer_capacity,
                            size_t nblock) override {

        int ret = 0;
        for (size_t i = 0; i < nblock; i++) {
            ret = compress(uncompressed_data[i], src_chunk_len[i], compressed_data[i],
                           dst_buffer_capacity / nblock);
            if (ret < 0) {
                LOG_ERROR_RETURN(EFAULT, -1, "ZSTD compress data failed. (retcode: `).", ret);
            }
            dst_chunk_len[i] = ret;
        }
        return 0;
    }

    virtual int do_decompress(size_t *src_chunk_len,
                              /* compressed length per block */ size_t *dst_chunk_len,
                              size_t dst_buffer_capacity, size_t nblock) override {

        int ret = 0;
        for (size_t i = 0; i < nblock; i++) {
            ret = decompress((const unsigned char *)uncompressed_data[i], src_chunk_len[i],
                             compressed_data[i], dst_buffer_capacity / nblock);

            dst_chunk_len[i] = ret;
            if (ret < 0) {
                LOG_ERROR_RETURN(EFAULT, -1, "LZ4 compress data failed. (retcode: `).", ret);
            }
            if (ret == 0) {
                LOG_ERROR_RETURN(
                    EFAULT, -1,
                    "Compression worked, but was stopped because the *dst couldn't hold all the information.");
            }
        }
        return 0;
    }

    virtual int decompress(const unsigned char *src, size_t src_len, unsigned char *dst,
                           size_t dst_len) override {
        if (dst_len < src_blk_size) {
            LOG_ERROR_RETURN(0, -1, "dst_len (`) should be greater than compressed block size `",
                             dst_len, src_blk_size);
        }

        size_t ret = ZSTD_decompress(dst, dst_len, src, src_len);
        if (ZSTD_isError(ret)) {
            LOG_ERROR_RETURN(0, -1, "decompress error: `", ZSTD_getErrorName(ret));
        }
        return ret;
    }
};

ICompressor *create_compressor(const CompressArgs *args) {
    ICompressor *rst = nullptr;
    int init_flg = 0;
    const CompressOptions &opt = args->opt;
    switch (opt.algo) {

    case CompressOptions::LZ4:
        rst = new LZ4Compressor;
        LOG_INFO("ZFileObject using LZ4 algorithm");
        if (rst != nullptr) {
            init_flg = ((LZ4Compressor *)rst)->init(args);
        }
        break;
    case CompressOptions::ZSTD:
        rst = new Compressor_zstd;
        LOG_INFO("ZFileObject using ZSTD algorithm");
        if (rst != nullptr) {
            init_flg = ((Compressor_zstd *)rst)->init(args);
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
