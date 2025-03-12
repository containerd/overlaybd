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

#include <photon/fs/filesystem.h>
#include <string.h>
#include <assert.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/estring.h>
#include <photon/common/string_view.h>
#include <unordered_map>
#include "elink.h"

namespace ELink {

static const int RAW_ALIGNED_SIZE = 1024;

struct TargetObject {
    std::string_view m_endpoint;
    std::string_view m_bucket_name;
    std::string_view source;
    std::string_view etag;
    size_t filesize = 0;

    TargetObject(std::string_view endpoint, std::string_view bucket_name, const char* raw_data, size_t size) : 
        m_endpoint(endpoint), m_bucket_name(bucket_name) {
        assert(size == RAW_ALIGNED_SIZE);
        auto p = raw_data;
        filesize = ((size_t*)raw_data)[0];
        p += sizeof(filesize);
        source = std::string_view(p, strlen(p));
        p += source.size() + 1;
        etag = std::string_view(p, strlen(p));
        LOG_DEBUG("parse target object. {source :` , size: `, etag: `}", source, filesize, etag);
    }

    estring remote_url() const {
        estring url;
        url.append("https://");
        url.append(m_bucket_name);
        url.append(".");
        url.append(m_endpoint);
        url.append(source);
        LOG_DEBUG("remote url: `", url);
        return url;
    }
};

class ICredentialClient {
public:
    virtual std::unordered_map<estring, estring> access_key(std::string_view url) = 0;
    virtual ~ICredentialClient() {};
};

class IAuthPlugin {
public:
    // virtual std::unordered_map<estring, estring> get_signed_info(const TargetObject &remote_url, std::string_view etag, size_t filesize) = 0;
    virtual photon::fs::IFile* get_signed_object(const TargetObject &t) = 0;
    virtual ~IAuthPlugin(){};
};


}