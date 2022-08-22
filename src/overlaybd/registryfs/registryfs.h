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
#include <stdint.h>
#include <string>
#include <photon/common/callback.h>
#include <photon/fs/virtual-file.h>

using namespace photon::fs;

// Docker registry always returns Token with expiry time >= 60 seconds.
// https://docs.docker.com/registry/spec/auth/token/#authorization-server-endpoint-descriptions
// static const uint64_t kMinimumTokenExpiry = 60 * 1000UL;  // 60 seconds

struct ImageLayerMeta {
    uint64_t crc64;
    uint64_t contentLength;
    char lastModified[128];
};

class RegistryFS : public IFileSystem {
public:
    virtual int setAccelerateAddress(const char* addr = "") = 0;
};

// IFile open by registryfs are RegistryFile, it can cast to RegistryFile* so
// that able to call getMeta & getUrl methods
// Since RegistryFile depends on registryfs to get authorized, it can only open
// using registryfs, and not able to create directly using url
class RegistryFile : public VirtualReadOnlyFile {
public:
    virtual int getMeta(ImageLayerMeta *meta, uint64_t timeout = -1) = 0;
    virtual int getUrl(char *buf, size_t size, uint64_t timeout = -1) = 0;
    int close() override {
        return 0;
    }
};

using PasswordCB = Delegate<std::pair<std::string, std::string>, const char *>;

extern "C" {
IFileSystem *new_registryfs_with_credential_callback(PasswordCB callback,
                                                     const char *caFile = nullptr,
                                                     uint64_t timeout = -1);
}

