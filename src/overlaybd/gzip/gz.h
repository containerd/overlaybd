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

#include <vector>
#include <photon/fs/filesystem.h>
#include <photon/fs/virtual-file.h>
#include <photon/net/socket.h>

class IGzFile :  public photon::fs::VirtualReadOnlyFile {
public :
    // return full filename of gzip index
    virtual std::string save_index() = 0;
    virtual std::string sha256_checksum() = 0;
};

photon::fs::IFile* open_gzfile_adaptor(const char *path);
IGzFile* open_gzstream_file(IStream *sock, ssize_t st_size,
    bool save_index = true, const char *uid = nullptr, const char *workdir = nullptr);
