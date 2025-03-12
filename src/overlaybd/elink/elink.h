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

#include <stdlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <photon/common/estring.h>
#include <photon/fs/filesystem.h>

namespace ELink{


struct TargetObject;

class ICredentialClient; 
class IAuthPlugin;

enum class AuthPluginType {
    AliyunOSS
};

class IReferenceList {
public:
    virtual photon::fs::IFile* get_remote_target(off_t target_index = -1) = 0;
    virtual ~IReferenceList();
};

IReferenceList *create_reference_list(photon::fs::IFile* file, std::string_view bucket_name, IAuthPlugin *auth);

ICredentialClient *create_simple_cred_client(const char *fn);
IAuthPlugin *create_auth_plugin(ICredentialClient *cred, AuthPluginType type);

photon::fs::IFile *open_elink_file(IReferenceList *reflist);

}