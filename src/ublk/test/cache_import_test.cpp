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

// Integration test for the managed Photon cache: local import, offline hit,
// automatic capacity eviction, and source fallback. No KVM or ublk is needed.
#include <dirent.h>
#include <openssl/sha.h>
#include <vector>
#include <algorithm>
#include <ftw.h>
#include "../../image_service.h"
#include "../ublkd_protocol.h"
#include <photon/fs/cache/cache.h>
#include <photon/fs/localfs.h>
#include <photon/fs/forwardfs.h>
#include <photon/photon.h>
#include <cassert>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include "../cache_import.h"

size_t cache_fn_trans_sha256(void *, std::string_view, char *, size_t);

class CountingSource : public photon::fs::ForwardFS_Ownership {
public:
    int opens = 0;
    explicit CountingSource(const char *root)
        : ForwardFS_Ownership(photon::fs::new_localfs_adaptor(root), true) {
    }
    photon::fs::IFile *open(const char *path, int flags, mode_t) override {
        return open(path, flags);
    }
    photon::fs::IFile *open(const char *path, int flags) override {
        ++opens;
        std::string name(path);
        name = name.substr(name.rfind('/') + 1);
        return m_fs->open(name.c_str(), flags);
    }
};

static std::string make_blob(const std::string &root, size_t size, unsigned char tag) {
    std::vector<unsigned char> block(256 * 1024, tag);
    SHA256_CTX hash;
    SHA256_Init(&hash);
    std::string tmp = root + "/input";
    std::ofstream out(tmp, std::ios::binary);
    for (size_t off = 0; off < size; off += block.size()) {
        auto n = std::min(block.size(), size - off);
        out.write((char *)block.data(), n);
        SHA256_Update(&hash, block.data(), n);
    }
    out.close();
    unsigned char sum[32];
    SHA256_Final(sum, &hash);
    std::string digest = "sha256:";
    for (auto c : sum) {
        digest += "0123456789abcdef"[c >> 4];
        digest += "0123456789abcdef"[c & 15];
    }
    assert(::rename(tmp.c_str(), (root + "/" + digest).c_str()) == 0);
    return digest;
}

static int import(ImageService &service, const std::string &path, const std::string &digest,
                  size_t size) {
    std::string body = "{\"path\":\"" + path + "\",\"url\":\"https://registry.test/v2/repo/blobs/" +
                       digest + "\",\"digest\":\"" + digest +
                       "\",\"size\":" + std::to_string(size) + "}";
    int code = 0;
    std::string msg;
    ublkd_handle_cache_import(&service, body, code, msg);
    if (code != 200)
        std::cout << "import status=" << code << " " << msg << std::endl;
    return code;
}

static int remove_entry(const char *path, const struct stat *, int, struct FTW *) {
    return ::remove(path);
}

int main() {
    assert(photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_DEFAULT) == 0);
    char root[] = "/tmp/cache-import-test-XXXXXX";
    assert(mkdtemp(root));
    {
        std::string source = std::string(root) + "/source", media = std::string(root) + "/cache";
        assert(::mkdir(source.c_str(), 0700) == 0);
        assert(::mkdir(media.c_str(), 0700) == 0);
        ImageService service;
        std::string configPath = std::string(root) + "/config.json";
        std::ofstream(configPath) << "{\"cacheConfig\":{\"cacheType\":\"file\"}}";
        service.global_conf.ParseJSON(configPath);
        auto src = new CountingSource(source.c_str());
        service.global_fs.srcfs = src;
        service.global_fs.io_alloc = new IOAlloc;
        auto cache = photon::fs::new_full_file_cached_fs(
            src, photon::fs::new_localfs_adaptor(media.c_str()), 256 * 1024, 1, 10000, 0,
            service.global_fs.io_alloc, 0, {nullptr, &cache_fn_trans_sha256}, 0);
        assert(cache);
        service.global_fs.cached_fs = cache;
        const size_t small = 1024 * 1024 + 17; // exercise a non-block-aligned tail
        auto first = make_blob(source, small, 42);
        assert(import(service, source + "/" + first, first, small) == 200);
        assert(src->opens == 0);
        // Reject invalid requests before touching the managed cache.
        int status = 0;
        std::string message;
        for (const auto &body : {"", "[]", "{}", "{\"path\":1}"}) {
            ublkd_handle_cache_import(&service, body, status, message);
            assert(status == 400);
        }
        for (const auto &fields :
             {"\"path\":\"relative\",\"url\":\"https://registry.test/v2/repo/blobs/",
              "\"path\":\"/tmp/blob\",\"url\":\"file:///blobs/"}) {
            std::string body =
                "{" + std::string(fields) + first + "\",\"digest\":\"" + first + "\",\"size\":1}";
            ublkd_handle_cache_import(&service, body, status, message);
            assert(status == 400);
        }
        assert(import(service, source + "/" + first, first, 0) == 400);
        assert(import(service, source + "/" + first, first, small + 1) == 400);
        assert(import(service, source, first, small) == 400);
        assert(import(service, source + "/missing", first, small) == 400);
        assert(::symlink((source + "/" + first).c_str(), (source + "/link").c_str()) == 0);
        assert(import(service, source + "/link", first, small) == 400);
        assert(::unlink((source + "/link").c_str()) == 0);
        assert(import(service, source + "/" + first, "sha256:bad", small) == 400);
        std::ofstream(configPath) << "{\"cacheConfig\":{\"cacheType\":\"ocf\"}}";
        service.global_conf.ParseJSON(configPath);
        assert(import(service, source + "/" + first, first, small) == 409);
        std::ofstream(configPath) << "{\"cacheConfig\":{\"cacheType\":\"file\"}}";
        service.global_conf.ParseJSON(configPath);
        assert(::unlink((source + "/" + first).c_str()) == 0);
        auto url = "https://registry.test/v2/repo/blobs/" + first;
        std::unique_ptr<photon::fs::IFile> file(cache->open(url.c_str(), O_RDONLY));
        std::vector<unsigned char> bytes(small);
        assert(file && file->pread(bytes.data(), small, 0) == (ssize_t)small);
        for (auto b : bytes)
            assert(b == 42);
        assert(src->opens == 0);
        std::cout << "PASS imported blob readable after staging deletion, source opens=0"
                  << std::endl;
        assert(make_blob(source, small, 42) == first);
        // A bad digest must not poison an existing cache entry.
        auto bad = make_blob(source, small, 43);
        assert(import(service, source + "/" + bad, first, small) == 400);
        assert(file->pread(bytes.data(), small, 0) == (ssize_t)small && bytes[0] == 42);
        std::cout << "PASS digest mismatch rejected before cache writes" << std::endl;
        // Drop the reader so normal LRU can reclaim this oldest entry.
        file.reset();
        for (int n = 0; n < 12; ++n) {
            auto digest = make_blob(source, 100 * 1024 * 1024, 60 + n);
            auto code = import(service, source + "/" + digest, digest, 100 * 1024 * 1024);
            assert(code == 200 || code == 503);
            ::unlink((source + "/" + digest).c_str());
            photon::thread_usleep(20000);
        }
        struct stat st;
        auto cachedPath = media + "/" + first;
        bool evicted = ::stat(cachedPath.c_str(), &st) != 0 || st.st_blocks == 0;
        assert(evicted);
        uint64_t used = 0;
        auto dir = ::opendir(media.c_str());
        assert(dir);
        while (auto entry = ::readdir(dir)) {
            if (entry->d_name[0] == '.')
                continue;
            struct stat entryStat;
            assert(::stat((media + "/" + entry->d_name).c_str(), &entryStat) == 0);
            used += entryStat.st_blocks * 512;
        }
        ::closedir(dir);
        assert(used <= 1024ULL * 1024 * 1024);
        std::cout << "PASS automatic capacity GC evicted imported blob, cache bytes=" << used
                  << std::endl;
        file.reset(cache->open(url.c_str(), O_RDONLY));
        assert(file && file->pread(bytes.data(), small, 0) == (ssize_t)small);
        for (auto b : bytes)
            assert(b == 42);
        assert(src->opens > 0);
        std::cout << "PASS evicted blob refills from source, source opens=" << src->opens
                  << std::endl;
        file.reset();
    }
    assert(::nftw(root, remove_entry, 32, FTW_DEPTH | FTW_PHYS) == 0);
    photon::fini();
}
