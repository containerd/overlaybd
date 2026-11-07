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
#include <functional>
#include <cerrno>
#include <algorithm>
#include <ftw.h>
#include "../../image_service.h"
#include "../ublkd_protocol.h"
#include <photon/fs/cache/cache.h>
#include <photon/fs/localfs.h>
#include <photon/fs/forwardfs.h>
#include <photon/photon.h>
#include <photon/thread/thread11.h>
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

// Deterministic fault boundaries: open happens after full digest validation;
// each pwrite happens after the corresponding source chunk is revalidated.
class ImportFaultFile : public photon::fs::ForwardFile_Ownership {
public:
    std::function<void()> after_first;
    bool fail_second;
    int writes = 0;
    ImportFaultFile(photon::fs::IFile *file, std::function<void()> hook, bool fail)
        : ForwardFile_Ownership(file, true), after_first(hook), fail_second(fail) {}
    ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        if (++writes == 2 && fail_second) {
            errno = EIO;
            return -1;
        }
        auto n = m_file->pwrite(buf, count, offset);
        if (writes == 1 && after_first) after_first();
        return n;
    }
};

class ImportFaultFS : public photon::fs::ForwardFS_Ownership {
public:
    std::function<void()> on_open, after_first;
    bool fail_second = false;
    explicit ImportFaultFS(photon::fs::IFileSystem *fs) : ForwardFS_Ownership(fs, false) {}
    photon::fs::IFile *open(const char *path, int flags, mode_t) override {
        return open(path, flags);
    }
    photon::fs::IFile *open(const char *path, int flags) override {
        if (on_open) on_open();
        auto file = m_fs->open(path, flags);
        return file ? new ImportFaultFile(file, after_first, fail_second) : nullptr;
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
        // P2P actually using srcfs bypasses these cached bytes: never report a
        // successful warmup for that read path. Oversized requests are bounded.
        service.global_fs.remote_fs = src;
        assert(import(service, source + "/" + first, first, small) == 409);
        service.global_fs.remote_fs = nullptr;
        assert(import(service, source + "/" + first, first,
                      64ULL * 1024 * 1024 * 1024 + 1) == 413);
        {
            ImportFaultFS fault(cache);
            service.global_fs.cached_fs = &fault;
            const auto staged = source + "/staged";
            auto prepare = [&](unsigned char tag) {
                auto digest = make_blob(source, small, tag);
                std::ifstream in(source + "/" + digest, std::ios::binary);
                std::ofstream out(staged, std::ios::binary);
                out << in.rdbuf();
                return digest;
            };
            auto overwrite = [&](off_t offset) {
                int fd = ::open(staged.c_str(), O_WRONLY);
                assert(fd >= 0);
                unsigned char changed = 255;
                assert(::pwrite(fd, &changed, 1, offset) == 1);
                assert(::close(fd) == 0);
            };
            auto read_back = [&](const std::string &digest, unsigned char tag,
                                 bool has_prefix = false) {
                auto name = "https://registry.test/v2/repo/blobs/" + digest;
                std::unique_ptr<photon::fs::IFile> reader(cache->open(name.c_str(), O_RDONLY));
                std::vector<unsigned char> data(small);
                assert(reader);
                if (has_prefix) {
                    int opens = src->opens;
                    assert(reader->pread(data.data(), 256 * 1024, 0) == 256 * 1024);
                    for (size_t i = 0; i < 256 * 1024; ++i) assert(data[i] == tag);
                    assert(src->opens == opens);
                }
                assert(reader->pread(data.data(), small, 0) == (ssize_t)small);
                for (auto b : data) assert(b == tag);
            };
            // An overlapping request is rejected without waiting. The outer
            // request and a subsequent retry must both remain usable.
            auto concurrent = prepare(80);
            fault.on_open = [&] {
                auto contender = photon::thread_create11([&] {
                    assert(import(service, staged, concurrent, small) == 429);
                });
                assert(contender);
                photon::thread_join(photon::thread_enable_join(contender));
            };
            assert(import(service, staged, concurrent, small) == 200);
            fault.on_open = nullptr;
            assert(import(service, staged, concurrent, small) == 200);
            read_back(concurrent, 80);

            // Mutation exactly between the two passes must publish no corrupt
            // bytes. A separately preserved remote blob supplies cache misses.
            auto changed = prepare(81);
            fault.on_open = [&] { overwrite(0); };
            assert(import(service, staged, changed, small) == 400);
            fault.on_open = nullptr;
            read_back(changed, 81);

            // Mutation after a verified prefix has been filled must leave that
            // prefix safe and allow the untouched source to refill the suffix.
            auto partial = prepare(82);
            fault.after_first = [&] { overwrite(256 * 1024); };
            assert(import(service, staged, partial, small) == 400);
            fault.after_first = nullptr;
            read_back(partial, 82, true);

            auto truncated = prepare(83);
            fault.after_first = [&] { assert(::truncate(staged.c_str(), 0) == 0); };
            assert(import(service, staged, truncated, small) == 400);
            fault.after_first = nullptr;
            read_back(truncated, 83, true);

            auto interrupted = prepare(84);
            fault.fail_second = true;
            assert(import(service, staged, interrupted, small) == 503);
            fault.fail_second = false;
            read_back(interrupted, 84, true);
            assert(import(service, staged, interrupted, small) == 200);
            service.global_fs.cached_fs = cache;
            assert(::unlink(staged.c_str()) == 0);
        }
        std::cout << "PASS admission, source mutation, truncation and write failure fallback"
                  << std::endl;
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
            assert(::stat((media + "/" + std::string(entry->d_name)).c_str(), &entryStat) == 0);
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
