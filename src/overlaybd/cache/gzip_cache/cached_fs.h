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
#include <photon/common/io-alloc.h>
#include <photon/fs/filesystem.h>
namespace Cache {

class GzipCachedFs {
public:
    virtual ~GzipCachedFs() {}
    virtual photon::fs::IFile *open_cached_gzip_file(photon::fs::IFile *file, const char *file_name) = 0;
};
GzipCachedFs *new_gzip_cached_fs(photon::fs::IFileSystem *mediaFs, uint64_t refillUnit,
                                            uint64_t capacityInGB, uint64_t periodInUs,
                                            uint64_t diskAvailInBytes, IOAlloc *allocator);
} // namespace Cache
