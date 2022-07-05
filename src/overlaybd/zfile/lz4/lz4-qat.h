/*
 *  LZ4 - Fast LZ compression algorithm
 *  Header File
 *  Copyright (C) 2011-present, Yann Collet.

   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   You can contact the author at :
    - LZ4 homepage : http://www.lz4.org
    - LZ4 source repository : https://github.com/lz4/lz4
*/
#if defined(__cplusplus)
extern "C" {
#endif

#ifndef LZ4_QAT_H_2983827168210
#define LZ4_QAT_H_2983827168210

#include<stddef.h>
#include<stdio.h>
/* --- Dependency --- */

#ifndef LZ4LIB_VISIBILITY
#if defined(__GNUC__) && (__GNUC__ >= 4)
#define LZ4LIB_VISIBILITY __attribute__((visibility("default")))
#else
#define LZ4LIB_VISIBILITY
#endif
#endif
#if defined(LZ4_DLL_EXPORT) && (LZ4_DLL_EXPORT == 1)
#define LZ4LIB_API __declspec(dllexport) LZ4LIB_VISIBILITY
#elif defined(LZ4_DLL_IMPORT) && (LZ4_DLL_IMPORT == 1)
#define LZ4LIB_API                                                                                 \
    __declspec(dllimport)                                                                          \
        LZ4LIB_VISIBILITY /* It isn't required but allows to generate better code, saving a        \
                             function pointer load from the IAT and an indirect jump.*/
#else
#define LZ4LIB_API LZ4LIB_VISIBILITY
#endif

typedef struct _LZ4_qat_param LZ4_qat_param;
struct _LZ4_qat_param {

};

/*-************************************
 *  Simple Functions
 **************************************/
/*! LZ4_compress_qat() :
    Compresses 'srcSize' bytes from buffer 'src'
    into already allocated 'dst' buffer of size 'dstCapacity'.
    Compression is guaranteed to succeed if 'dstCapacity' >= LZ4_compressBound(srcSize).
    It also runs faster, so it's a recommended setting.
    If the function cannot compress 'src' into a more limited 'dst' budget,
    compression stops *immediately*, and the function result is zero.
    Note : as a consequence, 'dst' content is not valid.
    Note 2 : This function is protected against buffer overflow scenarios (never writes outside
   'dst' buffer, nor read outside 'source' buffer). srcSize : max supported value is
   LZ4_MAX_INPUT_SIZE. dstCapacity : size of buffer 'dst' (which must be already allocated) return
   : the number of bytes written into buffer 'dst' (necessarily <= dstCapacity)
                  or 0 if compression fails */
LZ4LIB_API int LZ4_compress_qat(LZ4_qat_param *pQat, const unsigned char *const raw_data[],
                                size_t src_chunk_len[], unsigned char *compressed_data[],
                                size_t dst_chunk_len[], size_t n);

/*! LZ4_decompress_qat() :
    compressedSize : is the exact complete size of the compressed block.
    dstCapacity : is the size of destination buffer, which must be already allocated.
    return : the number of bytes decompressed into destination buffer (necessarily <= dstCapacity)
             If destination buffer is not large enough, decoding will stop and output an error code
   (negative value). If the source stream is detected malformed, the function will stop decoding and
   return a negative result. This function is protected against malicious data packets.
*/
LZ4LIB_API int LZ4_decompress_qat(LZ4_qat_param *pQat, const unsigned char *const raw_data[],
                                size_t src_chunk_len[], unsigned char *decompressed_data[],
                                size_t dst_chunk_len[], size_t n);


int qat_init(LZ4_qat_param *pQat);

int qat_uninit(LZ4_qat_param *pQat);

#endif /* LZ4_QAT_H_2983827168210 */

#if defined(__cplusplus)
}
#endif