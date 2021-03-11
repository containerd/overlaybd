/*
  * localfs.h
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
#include <sys/types.h>

namespace FileSystem
{
    class IFile;
    class IFileSystem;


    const int ioengine_psync = 0;

    const int ioengine_libaio = 1;          // libaio depends on photon::libaio_wrapper_init(),
                                            // and photon::fd-events ( fd_events_init() )

    const int ioengine_posixaio = 2;        // posixaio depends on photon::fd-events ( fd_events_init() )


    extern "C" IFileSystem* new_localfs_adaptor(const char* root_path = nullptr,
                                                int io_engine_type = 0);

    extern "C" IFile* new_localfile_adaptor(int fd, int io_engine_type = 0);

    extern "C" IFile* open_localfile_adaptor(const char* filename, int flags,
                                             mode_t mode = 0644, int io_engine_type = 0);

    inline __attribute__((always_inline))
    IFile* new_libaio_file_adaptor(int fd)
    {
        return new_localfile_adaptor(fd, ioengine_libaio);
    }

    inline __attribute__((always_inline))
    IFile* new_posixaio_file_adaptor(int fd)
    {
        return new_localfile_adaptor(fd, ioengine_posixaio);
    }
}
