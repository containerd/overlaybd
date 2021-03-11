/*
  * zfile.h
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

#pragma once

#include "compressor.h"
namespace ZFile
{
    const static size_t MAX_READ_SIZE     = 65536; // 64K
    
    extern "C" FileSystem::IFile* zfile_open_ro(FileSystem::IFile* file, bool verify = false,
                                            bool ownership = false);

    extern "C" int zfile_compress(  FileSystem::IFile* src_file, 
                    FileSystem::IFile* dst_file, 
                    const CompressArgs *opt = nullptr);

    extern "C" int zfile_decompress(FileSystem::IFile *src_file, FileSystem::IFile *dst_file);

    // return 1 if file object is a zfile.
    // return 0 if file object is a normal file.
    // otherwise return -1.
    extern "C" int is_zfile(FileSystem::IFile *file);
}
