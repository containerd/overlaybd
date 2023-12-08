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
#include "photon/fs/filesystem.h"
#include "gzfile_index.h"


extern photon::fs::IFile* new_gzfile(photon::fs::IFile* gzip_file, photon::fs::IFile* index, bool ownership = false);

//chunksize:
//1MB: 1048576
//2MB: 2097152
//3MB: 3145728
//4MB: 4194304

//dict_compress_algo:
//0: don't compress dictionary
//1: compress dictionary with zlib

//dict_compress_level:
//0: no compression
//1: best speed
//9: best compression
extern int create_gz_index(photon::fs::IFile* gzip_file, const char *index_file_path,
    off_t chunk_size=GZ_CHUNK_SIZE, int dict_compress_algo=GZ_DICT_COMPERSS_ALGO, int dict_compress_level=GZ_COMPRESS_LEVEL);

bool is_gzfile(photon::fs::IFile* file);
