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
#include <sys/types.h>

namespace FileSystem
{
    class IFile;
    class IFileSystem;


    const int ioengine_psync = 0;

    const int ioengine_libaio = 1;          // libaio depends on photon::libaio_wrapper_init(),
                                            // and photon::fd-events ( fd_events_init() )


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
}
