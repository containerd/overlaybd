/*
  * tar_zfile.h
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

namespace FileSystem {

    class IFileSystem;
    class IFile;
    extern "C" IFileSystem* new_tar_zfile_fs_adaptor(IFileSystem* fs);

    extern "C" int is_tar_file(FileSystem::IFile *file);
    extern "C" int is_tar_zfile(FileSystem::IFile *file);
    extern "C" IFile* new_tar_file_adaptor(FileSystem::IFile *file);
}