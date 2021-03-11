/*
  * compressor.h
  * 
  * Copyright (C) 2021 Alibaba Group.
  * 
  * This program is free software; you can redistribute it and/or
  * modify it under the terms of the GNU General Public License
  * as published by the Free Software Foundation; either version 2
  * of the License, or (at your option) any later version.
  * 
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  * 
  * See the file COPYING included with this distribution for more details.
*/
#ifndef __COMPRESSOR_H__
#define __COMPRESSOR_H__

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>

namespace FileSystem
{
    class IFile;
}


namespace ZFile{

    /* CompressOption will write into file */
    class CompressOptions
    {
    public:
        const static uint8_t MINI_LZO   = 0;
        const static uint8_t LZ4        = 1;
        const static uint8_t ZSTD       = 2;
        const static uint32_t DEFAULT_BLOCK_SIZE = 4096;//8192;//32768;

        uint32_t block_size = DEFAULT_BLOCK_SIZE;
        uint8_t type = LZ4;   // algorithm
        uint8_t level = 0;  // compress level
        uint8_t use_dict = 0;
        uint32_t args = 0;  // reserve;
        uint32_t dict_size = 0;
        uint8_t verify = 0;
        
        CompressOptions(uint8_t type = LZ4, 
                        uint32_t block_size = DEFAULT_BLOCK_SIZE,
                        uint8_t verify = 0) 
                : block_size(block_size), type(type), verify(verify)
        {}
    };

    class CompressArgs
    {
    public:
        FileSystem::IFile *fdict = nullptr;
        std::unique_ptr<unsigned char[]> dict_buf = nullptr;
        CompressOptions opt;

        CompressArgs(const CompressOptions &opt, FileSystem::IFile *dict = nullptr,
                    unsigned char *dict_buf = nullptr)
         :  fdict(dict), dict_buf(dict_buf), opt(opt)
        {
            if (fdict || dict_buf) {
                this->opt.use_dict = 1;
            }
        };
    };


    class ICompressor
    {
    public:
        virtual ~ICompressor(){};
        /*
            return compressed buffer size.
            return -1 when error occurred.
        */
        virtual int compress(const unsigned char *src, size_t src_len,
                        unsigned char *dst, size_t dst_len) = 0;
        /*
            return decompressed buffer size.
            return -1 when error occurred.
        */
        virtual int decompress(const unsigned char *src, size_t src_len,
                        unsigned char *dst, size_t dst_len) = 0;
    };

    extern "C" ICompressor *create_compressor(const CompressArgs *args);
}

#endif
