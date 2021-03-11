/*
  * subfs.h
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

namespace FileSystem
{
    class IFile;
    class IFileSystem;
    
    // create a view of sub tree of an underlay fs
    IFileSystem* new_subfs(IFileSystem* underlayfs, const char* base_path, bool ownership);
    
    // create a view of part of an underlay file
    // only for pread/v and pwrite/v
    IFileSystem* new_subfile(IFile* underlay_file, uint64_t offset, uint64_t length, bool ownership);
}
