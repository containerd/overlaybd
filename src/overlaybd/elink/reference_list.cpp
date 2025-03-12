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

#include "photon/common/alog-stdstring.h"
#include "photon/common/alog.h"
#include "photon/common/estring.h"
#include <cstddef>
#include <string_view>
#include <unordered_map>
#include "def.h"

using namespace std;
using namespace photon::fs;

namespace ELink {

class RawReferenceList : public IReferenceList {
public:
    /* FileFormat */
    // | filesize | sourcePath + '\0' | eTag + '\0'  | mountPath + '\0' |
    // | 8 bytes  | - bytes           | 32 + 1 bytes | - bytes          |

    photon::fs::IFile *m_file = nullptr;

    string m_endpoint;
    string m_bucket_name;
    IAuthPlugin *m_auth = nullptr;

    // todo expire time?
    unordered_map<off_t, IFile*> filepool;

    RawReferenceList(photon::fs::IFile *file, string_view bucket_name, IAuthPlugin *auth): 
        m_file(file), m_bucket_name(bucket_name), m_auth(auth)
    {}

    virtual photon::fs::IFile* get_remote_target(off_t target_index = -1) override {
        
        assert(target_index >= 0);
        if (filepool.find(target_index) != filepool.end()) {
            LOG_DEBUG("return opened file.");
            return filepool[target_index];
        }
        off_t offset = target_index * RAW_ALIGNED_SIZE;
        char buf[RAW_ALIGNED_SIZE];
        if (m_file->pread(buf, RAW_ALIGNED_SIZE, offset) != RAW_ALIGNED_SIZE) {
            LOG_ERRNO_RETURN(0, nullptr, "read reference list failed, idx: ` offset: `", target_index, offset);
        }
        auto targetObject = TargetObject(m_endpoint, m_bucket_name, buf, RAW_ALIGNED_SIZE);
        return m_auth->get_signed_object(targetObject);
        
    }
};
}