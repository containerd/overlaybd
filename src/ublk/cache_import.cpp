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

#include "cache_import.h"
#include "ublkd_protocol.h"
#include "../image_service.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <vector>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <photon/thread/thread.h>

void ublkd_handle_cache_import(ImageService *service, const std::string &body, int &code,
                               std::string &msg) {
    auto fail = [&](int status, const char *reason) {
        code = status;
        msg = ublkd_msg_error(reason);
    };
    rapidjson::Document doc;
    doc.Parse(body.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("path") ||
        !doc["path"].IsString() || !doc.HasMember("url") || !doc["url"].IsString() ||
        !doc.HasMember("digest") || !doc["digest"].IsString() || !doc.HasMember("size") ||
        !doc["size"].IsInt64() || doc["size"].GetInt64() <= 0) {
        fail(400, "expected path, url, sha256 digest and positive size");
        return;
    }
    std::string path(doc["path"].GetString(), doc["path"].GetStringLength());
    std::string url(doc["url"].GetString(), doc["url"].GetStringLength());
    std::string digest(doc["digest"].GetString(), doc["digest"].GetStringLength());
    const int64_t size = doc["size"].GetInt64();
    const std::string suffix = "/blobs/" + digest;
    if (digest.size() != 71 || digest.substr(0, 7) != "sha256:" ||
        digest.find_first_not_of("0123456789abcdef", 7) != std::string::npos || path.empty() ||
        path[0] != '/' || path.find('\0') != std::string::npos ||
        url.find('\0') != std::string::npos ||
        (url.substr(0, 7) != "http://" && url.substr(0, 8) != "https://") ||
        url.size() <= suffix.size() ||
        url.compare(url.size() - suffix.size(), suffix.size(), suffix) != 0) {
        fail(400, "invalid content-addressed cache import");
        return;
    }
    if (service->global_conf.cacheConfig().cacheType() != "file" || !service->global_fs.cached_fs) {
        fail(409, "local import requires the managed file cache");
        return;
    }
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        fail(400, "cannot open local blob");
        return;
    }
    DEFER(::close(fd));
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != size) {
        fail(400, "local blob size or type mismatch");
        return;
    }
    // Validate the whole file before exposing any bytes under its digest.
    std::vector<unsigned char> buf(256 * 1024);
    SHA256_CTX hash;
    SHA256_Init(&hash);
    for (int64_t off = 0; off < size;) {
        auto n = ::pread(fd, buf.data(), std::min<int64_t>(buf.size(), size - off), off);
        if (n <= 0) {
            fail(400, "cannot read local blob");
            return;
        }
        SHA256_Update(&hash, buf.data(), n);
        off += n;
        photon::thread_yield();
    }
    unsigned char sum[SHA256_DIGEST_LENGTH];
    SHA256_Final(sum, &hash);
    const char hex[] = "0123456789abcdef";
    std::string actual = "sha256:";
    for (auto b : sum) {
        actual += hex[b >> 4];
        actual += hex[b & 15];
    }
    if (actual != digest) {
        fail(400, "local blob digest mismatch");
        return;
    }
    // Keep normal read flags: O_CACHE_ONLY would also disable subsequent
    // remote cache-miss reads on a shared store. pwrite itself fills the cache.
    std::unique_ptr<photon::fs::IFile> cached(
        service->global_fs.cached_fs->open(url.c_str(), O_RDONLY));
    if (!cached || cached->ftruncate(size) != 0) {
        fail(503, "cannot open managed cache entry");
        return;
    }
    for (int64_t off = 0; off < size;) {
        const auto count = std::min<int64_t>(buf.size(), size - off);
        auto n = ::pread(fd, buf.data(), count, off);
        if (n != count || cached->pwrite(buf.data(), count, off) != count) {
            fail(503, "cache import incomplete; remote fallback remains available");
            return;
        }
        off += count;
        photon::thread_yield();
    }
    code = 200;
    msg = ublkd_msg_ok();
}
