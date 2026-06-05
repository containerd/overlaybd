/* lz4-qat.h — Intel QAT (qatlib) LZ4 batch decompress for ZFile */
#ifndef ZFILE_LZ4_QAT_H
#define ZFILE_LZ4_QAT_H

#include <cstddef>

#define ZFILE_QAT_RATIO_THRESHOLD_DEFAULT  1.5
#define ZFILE_QAT_POOL_SIZE                256
#define ZFILE_QAT_MAX_BLOCK_SIZE           65536

struct LZ4_qat_param;

int qat_init(LZ4_qat_param *p);
int qat_uninit(LZ4_qat_param *p);

/* Default ZFILE_QAT_RATIO_THRESHOLD_DEFAULT; env ZFILE_QAT_RATIO_THRESHOLD overrides during qat_init. */
void qat_set_ratio_threshold(LZ4_qat_param *p, double ratio);

/* Stub returning -1 until a QAT compress path lands; compressor.cpp falls through to CPU. */
int LZ4_compress_qat(LZ4_qat_param *p,
                     unsigned char **src, size_t *src_len,
                     unsigned char **dst, size_t *dst_len,
                     size_t n);

/* dst_len[i] in = capacity, out = decompressed bytes. n <= ZFILE_QAT_POOL_SIZE.
 * Returns 0 on QAT success, -1 on any QAT issue (caller falls through to CPU). */
int LZ4_decompress_qat(LZ4_qat_param *p,
                       unsigned char **src, size_t *src_len,
                       unsigned char **dst, size_t *dst_len,
                       size_t n);

/* Definition exposed in header because compressor.cpp does `new LZ4_qat_param()`.
 * The DMA buffer pool lives in lz4-qat.cpp as a process-global resource shared
 * across all params; per-param state below is only the QAT instance handle and
 * per-batch async counters. */
#include <atomic>
extern "C" {
#include "qat/cpa.h"
}

struct LZ4_qat_param {
    CpaInstanceHandle inst;
    double            ratio_threshold;
    std::atomic<int>  completed;
    std::atomic<int>  errors;

    LZ4_qat_param()
        : inst(nullptr),
          ratio_threshold(ZFILE_QAT_RATIO_THRESHOLD_DEFAULT),
          completed(0), errors(0) {}
};

#endif /* ZFILE_LZ4_QAT_H */
