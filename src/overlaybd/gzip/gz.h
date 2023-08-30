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
#include <photon/net/socket.h>

photon::fs::IFile* open_gzfile_adaptor(const char *path);
photon::fs::IFile* open_gzstream_file(IStream *sock, ssize_t st_size, photon::fs::IFile *save_idx_as = nullptr);

int save_gzip_index(photon::fs::IFile *gz_stream_file);
