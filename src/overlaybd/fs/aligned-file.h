/*
  * aligned-file.h
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
#include <inttypes.h>

struct AlignedAlloc;
struct IOAlloc;

namespace FileSystem
{
    class IFile;
    class IFileSystem;
    
    
    // create an adaptor to freely access a file that requires aligned access
    // alignment must be 2^n
    // only pread() and pwrite() are supported
    IFile* new_aligned_file_adaptor(IFile* file, uint32_t alignment,
                                    bool align_memory, bool ownership = false, 
                                    IOAlloc* allocator = nullptr);

    IFileSystem* new_aligned_fs_adaptor(IFileSystem* fs, uint32_t alignment,
                                        bool align_memory, bool ownership,
                                        IOAlloc* allocator = nullptr);
    
}
