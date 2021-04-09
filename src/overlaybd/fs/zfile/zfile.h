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
