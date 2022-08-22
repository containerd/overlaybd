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
#include "registryfs.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <photon/common/utility.h>
#include <photon/common/timeout.h>
#include <photon/common/object.h>
#include <photon/net/curl.h>
#include <photon/common/iovector.h>
#include <photon/common/identity-pool.h>
#include <photon/common/estring.h>
#include <photon/common/expirecontainer.h>
#include <photon/common/callback.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/alog.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>


static const estring kDockerRegistryAuthChallengeKeyValuePrefix = "www-authenticate";
static const estring kAuthHeaderKey = "Authorization";
static const estring kBearerAuthPrefix = "Bearer ";
static const estring kDockerRegistryBlobReaderFailPrefix = "DockerRegistryBolbReader Failure: ";
static const uint64_t kMinimalTokenLife = 30L * 1000 * 1000; // token lives atleast 30s
static const uint64_t kMinimalAUrlLife = 300L * 1000 * 1000; // actual_url lives atleast 300s
static const uint64_t kMinimalMetaLife = 300L * 1000 * 1000; // actual_url lives atleast 300s

static std::unordered_map<estring_view, estring_view> str_to_kvmap(const estring &src) {
    std::unordered_map<estring_view, estring_view> ret;
    for (const auto &token : src.split(',')) {
        auto pos = token.find_first_of('=');
        auto key = token.substr(0, pos);
        auto val = token.substr(pos + 1).trim('\"');
        ret.emplace(key, val);
    }
    return ret;
}

class RegistryFSImpl : public RegistryFS {
public:
    UNIMPLEMENTED_POINTER(IFile *creat(const char *, mode_t) override);
    UNIMPLEMENTED(int mkdir(const char *, mode_t) override);
    UNIMPLEMENTED(int rmdir(const char *) override);
    UNIMPLEMENTED(int link(const char *, const char *) override);
    UNIMPLEMENTED(int rename(const char *, const char *) override);
    UNIMPLEMENTED(int chmod(const char *, mode_t) override);
    UNIMPLEMENTED(int chown(const char *, uid_t, gid_t) override);
    UNIMPLEMENTED(int statfs(const char *path, struct statfs *buf) override);
    UNIMPLEMENTED(int statvfs(const char *path, struct statvfs *buf) override);
    UNIMPLEMENTED(int lstat(const char *path, struct stat *buf) override);
    UNIMPLEMENTED(int access(const char *pathname, int mode) override);
    UNIMPLEMENTED(int truncate(const char *path, off_t length) override);
    UNIMPLEMENTED(int syncfs() override);
    UNIMPLEMENTED(int unlink(const char *filename) override);
    UNIMPLEMENTED(int lchown(const char *pathname, uid_t owner, gid_t group) override);
    UNIMPLEMENTED_POINTER(DIR *opendir(const char *) override);
    UNIMPLEMENTED(int symlink(const char *meta_pathname, const char *pathname) override);
    UNIMPLEMENTED(ssize_t readlink(const char *filename, char *buf, size_t bufsize) override);

    virtual IFile *open(const char *pathname, int flags) override;

    virtual IFile *open(const char *pathname, int flags, mode_t) override {
        return open(pathname, flags); // ignore mode
    }

    RegistryFSImpl(PasswordCB callback, const char *caFile, uint64_t timeout)
        : m_callback(callback), m_caFile(caFile), m_timeout(timeout),
          m_meta_cache(kMinimalMetaLife), m_scope_token(kMinimalTokenLife),
          m_url_actual(kMinimalAUrlLife) {
    }

    ~RegistryFSImpl() {
    }

    long GET(const char *url, photon::net::HeaderMap *headers, off_t offset, size_t count,
             photon::net::IOVWriter *writer, uint64_t timeout) {
        Timeout tmo(timeout);
        long ret = 0;
        estring *actual_url = m_url_actual.acquire(url, [&]() -> estring * {
            estring *t = new estring();
            if (!getActualUrl(url, t, tmo.timeout(), ret)) {
                delete t;
                return nullptr;
            }
            return t;
        });
        if (actual_url == nullptr)
            return ret;

        //use p2p proxy
        if(m_accelerate.size() > 0) {
            estring accelerate_url = m_accelerate + "/" + *actual_url;
            actual_url = &accelerate_url;
            LOG_DEBUG("p2p_url: `", *actual_url);
        }

        {
            auto curl = get_cURL();
            DEFER({ release_cURL(curl); });
            curl->set_redirect(10);
            if (offset >= 0) {
                curl->set_range(offset, offset + count - 1);
            } else {
                curl->set_range(0, 0);
            }
            if (headers) {
                curl->set_header_container(headers);
            }
            // going to challenge
            if (writer) {
                ret = curl->GET(actual_url->c_str(), writer, tmo.timeout_us());
            } else {
                photon::net::DummyReaderWriter dummy;
                ret = curl->GET(actual_url->c_str(), &dummy, tmo.timeout_us());
            }
        }
        if (ret == 200 || ret == 206) {
            m_url_actual.release(url);
            return ret;
        } else {
            m_url_actual.release(url, true);
            LOG_ERROR_RETURN(0, ret, "Failed to fetch data, even authenticate passed ", VALUE(ret),
                             VALUE(url));
        }
    }

    int stat(const char *path, struct stat *buf) override {
        auto ctor = [&]() -> ImageLayerMeta * {
            auto meta = new ImageLayerMeta;
            auto file = (RegistryFile *)open(path, 0);
            DEFER(delete file);
            int ret = file->getMeta(meta, m_timeout);
            if (ret < 0) {
                delete meta;
                return nullptr;
            }
            return meta;
        };
        auto meta = m_meta_cache.acquire(path, ctor);
        if (meta == nullptr)
            return -1;
        DEFER(m_meta_cache.release(path));
        memset(buf, 0, sizeof(*buf));
        buf->st_mode = S_IFREG | S_IREAD;
        buf->st_size = meta->contentLength;
        return 0;
    }

    int getActualUrl(const char *url, estring *actual, uint64_t timeout, long &code) {
        Timeout tmo(timeout);
        auto curl = get_cURL();
        DEFER({ release_cURL(curl); });
        photon::net::HeaderMap headers;
        photon::net::DummyReaderWriter dummy;
        long ret = 0;

        estring authurl, scope;
        estring *token = nullptr;
        if (getScopeAuth(url, &authurl, &scope, tmo.timeout()) < 0)
            return false;

        if (!scope.empty()) {
            token = m_scope_token.acquire(scope, [&]() -> estring * {
                estring *token = new estring();
                auto ret = m_callback(url);
                if (!authenticate(authurl.c_str(), ret.first, ret.second, token, tmo.timeout())) {
                    code = 401;
                    delete token;
                    return nullptr;
                }
                return token;
            });
            if (token == nullptr)
                LOG_ERROR_RETURN(0, false, "Failed to get token");
        }
        curl->set_redirect(0).set_nobody().set_header_container(&headers);
        if (token)
            curl->append_header(kAuthHeaderKey, kBearerAuthPrefix + *token);
        // going to challenge
        ret = curl->GET(url, &dummy, tmo.timeout_us());
        code = ret;
        if (ret == 401 || ret == 403) {
            LOG_WARN("Token invalid, try refresh password next time");
        }
        if (300 <= ret && ret < 400) {
            // pass auth, redirect to source
            auto url = curl->getinfo<char *>(CURLINFO_REDIRECT_URL);
            *actual = url;
            if (!scope.empty())
                m_scope_token.release(scope);
            return true;
        }
        if (ret == 200) {
            *actual = url;
            if (!scope.empty())
                m_scope_token.release(scope);
            return true;
        } else {
            // unexpected situation
            if (!scope.empty())
                m_scope_token.release(scope, true);
            LOG_ERROR_RETURN(0, false, "Failed to get actual url ", VALUE(url), VALUE(ret));
        }
    }

    virtual int setAccelerateAddress(const char* addr = "") override {
        m_accelerate = estring(addr);
        return 0;
    }

protected:
    using CURLPool = IdentityPool<photon::net::cURL, 4>;
    CURLPool m_curl_pool;
    PasswordCB m_callback;
    estring m_accelerate;
    estring m_caFile;
    uint64_t m_timeout;
    ObjectCache<estring, ImageLayerMeta *> m_meta_cache;
    ObjectCache<estring, estring *> m_scope_token;
    ObjectCache<estring, estring *> m_url_actual;

    photon::net::cURL *get_cURL() {
        auto curl = m_curl_pool.get();
        curl->reset_error();
        curl->reset().clear_header().set_cafile(m_caFile.c_str());
        return curl;
    };

    void release_cURL(photon::net::cURL *curl) {
        m_curl_pool.put(curl);
    };

    int getScopeAuth(const char *url, estring *authurl, estring *scope, uint64_t timeout) {
        // need authorize;
        Timeout tmo(timeout);
        auto curl = get_cURL();
        DEFER({ release_cURL(curl); });
        photon::net::HeaderMap headers;
        photon::net::DummyReaderWriter dummy;
        curl->set_redirect(0).set_nobody().set_header_container(&headers);
        // going to challenge
        auto ret = curl->GET(url, &dummy, tmo.timeout_us());
        if (ret != 401 && ret != 403) {
            // no token request accepted, seems token not need;
            return 0;
        }
        if (!getAuthUrl(&headers, authurl, scope)) {
            LOG_ERROR_RETURN(0, -1, "Failed to get auth url.");
        }
        return 0;
    }

    int parseToken(const estring &jsonStr, estring *token) {
        rapidjson::Document d;
        if (d.Parse(jsonStr.c_str()).HasParseError())
            LOG_ERROR_RETURN(0, -1, "JSON parse failed");
        if (d.HasMember("token"))
            *token = d["token"].GetString();
        else if (d.HasMember("access_token"))
            *token = d["access_token"].GetString();
        else
            LOG_ERROR_RETURN(0, -1, "JSON has no 'token' or 'access_token' member");
        LOG_DEBUG("Get token", VALUE(*token));
        return 0;
    }

    bool authenticate(const char *auth_url, std::string &username, std::string &password,
                      estring *token, uint64_t timeout) {
        Timeout tmo(timeout);
        photon::net::cURL *req = get_cURL();
        DEFER({ release_cURL(req); });
        photon::net::StringWriter writer;
        if (!username.empty()) {
            req->set_user_passwd(username.c_str(), password.c_str()).set_redirect(3);
        }
        auto ret = req->GET(auth_url, &writer, tmo.timeout_us());

        LOG_DEBUG(VALUE(writer.string));

        if (ret == 200 && parseToken(writer.string, token) == 0) {
            return true;
        } else {
            LOG_ERROR_RETURN(0, false, "AUTH failed, response code=` ", ret, VALUE(auth_url));
        }
    }

    bool getAuthUrl(const photon::net::HeaderMap *headers, estring *auth_url, estring *scope) {
        auto it = headers->find(kDockerRegistryAuthChallengeKeyValuePrefix);
        if (it == headers->end())
            LOG_ERROR_RETURN(EINVAL, false, "no auth header in response");
        estring challengeLine = it->second;

        if (!challengeLine.starts_with(kBearerAuthPrefix))
            LOG_ERROR_RETURN(EINVAL, false, "auth string shows not bearer auth, ",
                             VALUE(challengeLine));
        challengeLine = challengeLine.substr(kBearerAuthPrefix.length());

        auto kv = str_to_kvmap(challengeLine);
        if (kv.find("realm") == kv.end() || kv.find("service") == kv.end() ||
            kv.find("scope") == kv.end()) {
            LOG_ERROR_RETURN(EINVAL, false, "authentication challenge failed with `",
                             challengeLine);
        }
        *scope = estring(kv["scope"]);
        *auth_url = estring(kv["realm"]) + "?service=" + estring(kv["service"]) +
                    "&scope=" + estring(kv["scope"]);
        return true;
    }
}; // namespace FileSystem

class RegistryFileImpl : public RegistryFile {
public:
    estring m_filename;
    estring m_url;
    RegistryFSImpl *m_fs;
    uint64_t m_timeout;
    size_t m_filesize;

    RegistryFileImpl(const char *filename, const char *url, RegistryFSImpl *fs, uint64_t timeout)
        : m_filename(filename), m_url(url), m_fs(fs), m_timeout(timeout) {
        m_filesize = 0;
    }

    virtual IFileSystem *filesystem() override {
        return m_fs;
    }

    ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) {
        if (m_filesize == 0) {
            struct stat stat;
            auto stret = fstat(&stat);
            if (stret < 0)
                return -1;
            m_filesize = stat.st_size;
        }
        auto filesize = m_filesize;
        int retry = 3;
        Timeout timeout(m_timeout);

    again:
        photon::net::IOVWriter container(iov, iovcnt);
        auto count = container.sum();
        if ((ssize_t)count + offset > filesize)
            count = filesize - offset;
        LOG_DEBUG("pulling blob from docker registry: ", VALUE(m_url), VALUE(offset), VALUE(count));

        photon::net::HeaderMap headers;
        long code =
            m_fs->GET(m_url.c_str(), &headers, offset, count, &container, timeout.timeout());

        if (code != 200 && code != 206) {
            ERRNO eno;
            if (timeout.expire() < photon::now) {
                LOG_ERROR_RETURN(ETIMEDOUT, -1, "timed out in preadv ", VALUE(m_url),
                                 VALUE(offset));
            }
            if (retry--) {
                ssize_t ret_len;
                for (auto &line : headers) {
                    LOG_DEBUG(VALUE(line.first), VALUE(line.second));
                }
                LOG_WARN("failed to perform HTTP GET, going to retry ", VALUE(code), VALUE(offset),
                         VALUE(count), VALUE(ret_len), eno);

                photon::thread_usleep(1000);
                goto again;
            } else {
                LOG_ERROR_RETURN(ENOENT, -1, "failed to perform HTTP GET ", VALUE(m_url),
                                 VALUE(offset));
            }
        }
        ssize_t ret = count;
        for (auto &line : headers) {
            LOG_DEBUG(VALUE(line.first), VALUE(line.second));
        }
        headers.try_get("content-length", ret);
        return ret;
    }

    /**
     * read meta data for the docker image layer. E.g. the content length in
     * bytes
     */
    virtual int getMeta(ImageLayerMeta *meta, uint64_t timeout) override {
        photon::net::HeaderMap headers;
        Timeout tmo(timeout);
        int retry = 3;
    again:
        auto code = m_fs->GET(m_url.c_str(), &headers, -1, -1, nullptr, tmo.timeout());
        if (code != 200 && code != 206) {
            if (tmo.expire() < photon::now)
                LOG_ERROR_RETURN(ETIMEDOUT, -1, "get meta timedout");

            if (code == 401 || code == 403) {
                if (retry--)
                    goto again;
                LOG_ERROR_RETURN(EPERM, -1, "Authorization failed");
            }
            if (retry--)
                goto again;
            LOG_ERROR_RETURN(ENOENT, -1, "failed to get meta from server");
        }
        char buffer[64];
        if (headers.try_get("content-range", buffer) < 0) { // part of data must have content-range
            if (headers.try_get("content-length", meta->contentLength) <
                0) { // or this is full data
                LOG_ERROR_RETURN(EIO, -1, "unexpected response header returned from head request");
            }
        } else { // if there is part data, get last number as result
            estring es(buffer);
            auto p = es.view().find_last_of(charset('/'));
            if (p == estring::npos) { // if not as standard http header
                LOG_ERROR_RETURN(EIO, -1, "unexpected response header content range");
            }
            meta->contentLength = std::atoll(es.data() + p + 1);
        }
        // optional
        headers.try_get("x-oss-hash-crc64ecma", meta->crc64);
        headers.try_get("last modified", meta->lastModified);
        return 0;
    }

    virtual int fstat(struct stat *buf) override {
        if (m_filesize == 0) {
            auto meta = new ImageLayerMeta;
            DEFER(delete meta);
            int ret = getMeta(meta, m_timeout);
            if (ret < 0) {
                return -1;
            }
            m_filesize = meta->contentLength;
        }
        memset(buf, 0, sizeof(*buf));
        buf->st_mode = S_IFREG | S_IREAD;
        buf->st_size = m_filesize;
        return 0;
    }

    /**
     * get redirect url address
     */
    virtual int getUrl(char *buf, size_t size, uint64_t timeout) override {
        estring aurl;
        long code;
        auto ret = m_fs->getActualUrl(m_url.c_str(), &aurl, timeout, code);
        if (ret) {
            strncpy(buf, aurl.c_str(), size);
            return 0;
        } else {
            return -1;
        }
    }
};

inline IFile *RegistryFSImpl::open(const char *pathname, int) {
    std::string url = pathname;
    std::string path = pathname;
    if (*pathname != '/')
        path = std::string("/") + pathname;

    auto file = new RegistryFileImpl(path.c_str(), url.c_str(), (RegistryFSImpl *)this, m_timeout);
    struct stat buf;
    int ret = file->fstat(&buf);
    if (ret < 0) {
        delete file;
        LOG_ERROR_RETURN(0, nullptr, "failed to open and stat registry file `, ret `", pathname,
                         ret);
    }
    return file;
}

IFileSystem *new_registryfs_with_credential_callback(PasswordCB callback, const char *caFile,
                                                     uint64_t timeout) {
    if (!callback)
        LOG_ERROR_RETURN(EINVAL, nullptr, "password callback not set");
    return new RegistryFSImpl(callback, caFile ? caFile : "", timeout);
}

