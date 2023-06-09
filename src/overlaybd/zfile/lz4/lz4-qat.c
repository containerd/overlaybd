#include "lz4.h"
#include "lz4-qat.h"
#include <unistd.h>

#define sleeptime 100

int gDebugParam = 1;


int qat_init(LZ4_qat_param *pQat) {
    int32_t status = 0;

    return (int)status;
}

int qat_uninit(LZ4_qat_param *pQat) {
    int32_t status = 0;

    return status;
}

// compression operation.

LZ4LIB_API int LZ4_compress_qat(LZ4_qat_param *pQat, const unsigned char *const raw_data[],
                                size_t src_chunk_len[], unsigned char *compressed_data[],
                                size_t dst_chunk_len[], size_t n) {
    int32_t status = 0;
    for (size_t i = 0; i < n; i++) {
        int ret = LZ4_compress_default((const char *)raw_data[i], (char *)compressed_data[i],
                                        src_chunk_len[i], 4096);
        dst_chunk_len[i] = ret;
    }
    return status;
}

LZ4LIB_API int LZ4_decompress_qat(LZ4_qat_param *pQat, const unsigned char *const raw_data[],
                                size_t src_chunk_len[], unsigned char *decompressed_data[],
                                size_t dst_chunk_len[], size_t n){
    int32_t status = 0;
    for (size_t i = 0; i < n; i++) {
        int ret = LZ4_decompress_safe((const char *)raw_data[i], (char *)decompressed_data[i],
                                        src_chunk_len[i], 4096);
        dst_chunk_len[i] = ret;
    }
    return status;
}
