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

#include "def.h"
#include "photon/common/alog.h"
#include "photon/fs/filesystem.h"
#include "../lsmt/index.h"
#include "photon/fs/virtual-file.h"
#include <errno.h>

using namespace photon::fs;
using namespace std;

namespace ELink{

class ELinkFile : public photon::fs::VirtualReadOnlyFile
{
public:
    IReferenceList *m_reflist = nullptr;
    // unordered_map<off_t, IFile*> m_files;

    ELinkFile(IReferenceList *reflist) : m_reflist(reflist){};

    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {

        LSMT::SegmentMapping m {
            offset / ALIGNMENT_4K, 
            (uint32_t)(count / ALIGNMENT_4K),
            0, 0
        };
        off_t ref_idx = m.reference_index();
        off_t inner_offset = m.inner_offest();
        assert(ref_idx>=0);
        auto file = m_reflist->get_remote_target(ref_idx);
        if (file == nullptr) {
            LOG_ERRNO_RETURN(EACCES, -1, "failed to get remote file");
        }
        return file->pread(buf, count, inner_offset);
    };

    UNIMPLEMENTED_POINTER(IFileSystem* filesystem() override);
    UNIMPLEMENTED(int fstat(struct stat *st) override);

};

photon::fs::IFile *open_elink_file(IReferenceList *reflist) {

    return new ELinkFile(reflist);
}
}