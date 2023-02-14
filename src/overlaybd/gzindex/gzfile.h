#pragma once
#include "photon/fs/filesystem.h"
extern photon::fs::IFile* new_gzfile(photon::fs::IFile* gzip_file, photon::fs::IFile* index);

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
extern int create_gz_index(photon::fs::IFile* gzip_file, const char *index_file_path, off_t chunk_size=1048576, int dict_compress_algo=1, int dict_compress_level=6);
