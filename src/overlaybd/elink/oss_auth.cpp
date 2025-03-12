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
#include "config.h"
#include "photon/common/alog.h"
#include "photon/fs/filesystem.h"
#include "yaml-cpp/node/node.h"

#include <fcntl.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <photon/photon.h>
#include <photon/thread/timer.h>
#include <photon/net/utils.h>
#include <photon/net/http/client.h>
#include <photon/net/http/message.h>
#include <photon/fs/httpfs/httpfs.h>
#include <unordered_map>

using namespace photon::net::http;
using namespace photon::fs;

namespace ELink{

class OSSSimpleCredentialClient : public ICredentialClient {
public:

    App::OssCredential c;
    OSSSimpleCredentialClient(const char *fn){
        auto node = YAML::LoadFile(fn);
        c = App::mergeConfig(c, node);
    }

    ~OSSSimpleCredentialClient(){}
    virtual std::unordered_map<estring, estring> access_key(std::string_view url) override {
        // TODO: maybe it needs to return different accessKey by 'url'
        std::unordered_map<estring, estring> ret;
        ret["access_key_id"] = c.accessKeyID();
        ret["access_key_secret"] = c.accessKeySecret();
        return ret;
    }
};


class OSSAuthPlugin : public ELink::IAuthPlugin 
{
public:

    IFileSystem *m_fs = nullptr; //httpfs

    ICredentialClient *m_client = nullptr;
    estring access_key_id;
    estring access_key_secret;

    OSSAuthPlugin(ICredentialClient *client) : m_client(client) {
        m_fs = new_httpfs_v2();
        
    }
    std::string hmac_sha1(std::string_view key, std::string_view data) {
        unsigned char output[EVP_MAX_MD_SIZE];
        unsigned int output_length;

    #if !defined(OPENSSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER < 0x10100000L
        HMAC_CTX ctx;
        auto evp_md = EVP_sha1();
        HMAC_CTX_init(&ctx);
        HMAC_Init_ex(&ctx, (const unsigned char *)key.data(), key.length(), evp_md, nullptr);
        HMAC_Update(&ctx, (const unsigned char *)data.data(), data.length());
        HMAC_Final(&ctx, (unsigned char *)output, &output_length);
        HMAC_CTX_cleanup(&ctx);
    #else
        HMAC_CTX *ctx = HMAC_CTX_new();
        auto evp_md = EVP_sha1();
        HMAC_CTX_reset(ctx);
        HMAC_Init_ex(ctx, (const unsigned char *)key.data(), key.length(), evp_md, nullptr);
        HMAC_Update(ctx, (const unsigned char *)data.data(), data.length());
        HMAC_Final(ctx, (unsigned char *)output, &output_length);
        HMAC_CTX_free(ctx);
    #endif
        return std::string((const char *)output, output_length);
    }

    void reload_access_key(std::string_view url) {
        auto r = m_client->access_key(url);
        access_key_id = r["access_key_id"];
        access_key_secret = r["access_key_secret"];
    }

    virtual IFile* get_signed_object(const TargetObject &target) override {
        char m_gmt_date[64]{};
        time_t t = photon::now / 1000 / 1000;
        struct tm *p = gmtime(&t);
        strftime(m_gmt_date, 64, "%a, %d %b %Y %H:%M:%S %Z", p);

        std::string sig;
        /* /dadi-shared.oss-cn-beijing.aliyuncs.com/k8s.gcr.io-pause-3.5.tar.gz */
        auto data = estring().appends("GET", "\n", "\n", "\n", m_gmt_date, "\n", "/",
                                    target.m_bucket_name, target.source, "");

        LOG_INFO(VALUE(data));
        if (access_key_id.empty()) {
            reload_access_key(target.remote_url());
        }
        LOG_DEBUG("acess_key_id: `, access_key_secret: `", access_key_id, access_key_secret);
        photon::net::Base64Encode(hmac_sha1(access_key_secret, data), sig);
        LOG_INFO(VALUE(m_gmt_date));
        auto a = estring().appends("OSS ", access_key_id, ":", sig);

        auto url = target.remote_url();
        auto remotefile = m_fs->open(url.c_str(), O_RDONLY);
        if (remotefile == nullptr) {
            LOG_ERRNO_RETURN(0, nullptr, "open remote file failed");
        }
        remotefile->ioctl(HTTP_HEADER, "Date", m_gmt_date);
        remotefile->ioctl(HTTP_HEADER, "Authorization", a.c_str());
        LOG_DEBUG("open remote object with headers [{Date: `}, {Authorization: `}]", m_gmt_date, a);
        // todo ETAG check
        struct stat st;
        if (remotefile->fstat(&st) != 0) {
            delete remotefile;
            LOG_ERRNO_RETURN(0, nullptr, "fstat remote target failed {path `}", target.source);
        }
        if ((size_t)st.st_size != target.filesize) {
            delete remotefile;
            LOG_ERRNO_RETURN(0, nullptr, "unexpected object size get {path `, size: `(!=`)}", target.source, st.st_size, target.filesize);
        }
        return remotefile;
    }

};

IAuthPlugin *create_auth_plugin(ICredentialClient *client, AuthPluginType type) {

    IAuthPlugin *ret = nullptr;
    switch (type) {
        case AuthPluginType::AliyunOSS:
            ret = new OSSAuthPlugin(client);
            return ret;
        default:
            LOG_ERRNO_RETURN(0, nullptr, "Unsupported auth type");
    }
    return nullptr;
}

ICredentialClient *create_simple_cred_client(const char *fn) {
    return new OSSSimpleCredentialClient(fn);
}


}

