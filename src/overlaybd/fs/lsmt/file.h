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

/*
VirualReadOnly -> IFileRO -> IFileRW -> LSMTReadOnlyFile -> LSMTFile

IMemoryIndex -> IMemoryIndex0 -> IComboIndex -> Index0 ( set<SegmentMap> ) -> ComboIndex
         |
         | -> Index ( vector<SegmentMap> )
*/

#pragma once
#include <inttypes.h>
#include <cstddef>
#include "../filesystem.h"
#include "../virtual-file.h"
#include "index.h"
#include "../../uuid.h"

namespace LSMT {

static const int MAX_STACK_LAYERS = 255;

typedef ::FileSystem::IFile IFile;
class IFileRO : public ::FileSystem::VirtualReadOnlyFile {
public:
    // set MAX_IO_SIZE of per read/write operation.
    virtual int set_max_io_size(size_t) = 0;
    virtual size_t get_max_io_size() = 0;

    virtual IMemoryIndex *index() const = 0;

    // return uuid of  m_files[layer_idx];
    virtual int get_uuid(UUID &out, size_t layer_idx = 0) const = 0;
};

struct CommitArgs {
    IFile *as = nullptr;
    char *user_tag = nullptr; // commit_msg, at most 256B
    size_t tag_len = 0;       // commit_msg length
    UUID::String parent_uuid; // set parent uuid when commit
    size_t get_tag_len() const {
        if (tag_len == 0 && user_tag != nullptr) {
            return strlen(user_tag);
        }
        return tag_len;
    }
    CommitArgs(IFile *as) : as(as){};
};

class IFileRW : public IFileRO {
public:
    virtual IMemoryIndex0 *index() const override = 0;

    const int Index_Group_Commit = 10;
    int set_index_group_commit(size_t buffer_size) {
        return this->ioctl(Index_Group_Commit, buffer_size);
    }

    // commit the written content as a new file, without garbages
    // return 0 for success, -1 otherwise
    virtual int commit(const CommitArgs &args) const = 0;
    // virtual int commit(IFile* as) const = 0;

    // close and seal current file, optionally returning a new
    // read-only file, with ownership of underlaying file transferred
    virtual int close_seal(IFileRO **reopen_as = nullptr) = 0;

    // data_stat returns data usage amount of the top RW layer as a 'DataStat' object.
    struct DataStat {
        uint64_t total_data_size = -1; // size of total data
        uint64_t valid_data_size = -1; // size of valid data (excluding garbage)
    };
    virtual DataStat data_stat() const = 0;
};

// create a new writable LSMT file constitued by a data file and an index file,
// optionally obtaining the ownerships of the underlying files,
// thus they will be destructed automatically.
struct LayerInfo {
    IFile *fdata = nullptr;
    IFile *findex = nullptr;
    uint64_t virtual_size;
    UUID parent_uuid;
    UUID uuid;
    char *user_tag = nullptr; // a user provided string of message, 256B at most
    size_t len = 0;           // len of user_tag; if it's 0, it will be detected with strlen()
    LayerInfo(IFile *_fdata = nullptr, IFile *_findex = nullptr) : fdata(_fdata), findex(_findex) {
        parent_uuid.clear();
        uuid.generate();
    }
};
extern "C" IFileRW *create_file_rw(const LayerInfo &args, bool ownership = false);

// open a writable LSMT file constitued by a data file and a index file,
// optionally obtaining the ownerships of the underlying files,
// thus they will be destructed automatically.
extern "C" IFileRW *open_file_rw(IFile *fdata, IFile *findex, bool ownership = false);

// open a read-only LSMT file, which was created by
// `close_seal()`ing or `commit()`ing a R/W LSMT file.
// optionally obtaining the `ownership` of the underlying file,
// thus it will be destructed automatically.
extern "C" IFileRO *open_file_ro(IFile *file, bool ownership = false);

// open a read-only (sealed) LSMT file constituted by multiple layers,
// with `files[0]` being the lowest layer, and vice versa
// optionally obtaining the ownerships of the underlying files,
// thus they will be destructed automatically.
extern "C" IFileRO *open_files_ro(IFile **files, size_t n, bool ownership = false);

// merge multiple RO files (layers) into a single RO file (layer)
// returning 0 for success, -1 otherwise
// extern "C" int merge_files_ro(IFile** src_files, size_t n, IFile* dest_file);
extern "C" int merge_files_ro(IFile **src_files, size_t n, const CommitArgs &args);

// stack a R/W layer (`upper_layer`) and a read-only layere (`lower_layer`)
// together, forming a virtual single R/W file
// optionally obtaining the ownerships of the underlying files,
// thus they will be destructed automatically.
extern "C" IFileRW *stack_files(IFileRW *upper_layer, IFileRO *lower_layers, bool ownership = false,
                                bool check_order = true);

} // namespace LSMT
