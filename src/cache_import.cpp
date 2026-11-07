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

#include "image_service.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <openssl/sha.h>
#include <photon/thread/thread.h>

namespace {
// POSIX reads may be short or interrupted, including reads of regular files.
ssize_t read_chunk(int fd, unsigned char *buffer, size_t count, off_t offset) {
    size_t done = 0;
    while (done < count) {
        auto n = ::pread(fd, buffer + done, count - done, offset + done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += n;
    }
    return done;
}
} // namespace

CacheImportResult ImageService::import_local_blob(const std::string &path,
    const std::string &url, const std::string &digest, int64_t size) {
    const std::string suffix = "/blobs/" + digest;
    if (size <= 0 || digest.size() != 71 || digest.substr(0, 7) != "sha256:" ||
        digest.find_first_not_of("0123456789abcdef", 7) != std::string::npos || path.empty() ||
        path[0] != '/' || path.find('\0') != std::string::npos ||
        url.find('\0') != std::string::npos ||
        (url.substr(0, 7) != "http://" && url.substr(0, 8) != "https://") ||
        url.size() <= suffix.size() ||
        url.compare(url.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return {CacheImportStatus::invalid, "invalid content-addressed cache import"};
    }
    // Bound proof memory to 8 MiB and reject overload instead of building an
    // unbounded queue of expensive synchronous imports.
    if (size > 64LL * 1024 * 1024 * 1024)
        return {CacheImportStatus::too_large, "local blob exceeds 64 GiB import limit"};
    if (m_cache_import_active.test_and_set())
        return {CacheImportStatus::busy, "another local blob import is active; retry later"};
    DEFER(m_cache_import_active.clear());
    if (global_conf.cacheConfig().cacheType() != "file" || !global_fs.cached_fs)
        return {CacheImportStatus::unsupported, "local import requires the managed file cache"};
    if (global_fs.remote_fs && global_fs.remote_fs != global_fs.cached_fs)
        return {CacheImportStatus::unsupported, "active remote read path bypasses the managed cache"};
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return {CacheImportStatus::invalid, "cannot open local blob"};
    }
    DEFER(::close(fd));
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != size) {
        return {CacheImportStatus::invalid, "local blob size or type mismatch"};
    }
    // Validate the whole file before exposing any bytes under its digest.
    std::vector<unsigned char> buf(256 * 1024);
    std::vector<std::array<unsigned char, SHA256_DIGEST_LENGTH>> chunks;
    chunks.reserve((size + buf.size() - 1) / buf.size());
    SHA256_CTX hash;
    SHA256_Init(&hash);
    for (int64_t off = 0; off < size;) {
        const auto count = std::min<int64_t>(buf.size(), size - off);
        auto n = read_chunk(fd, buf.data(), count, off);
        if (n != count) {
            return {CacheImportStatus::invalid, "cannot read local blob"};
        }
        SHA256_Update(&hash, buf.data(), n);
        std::array<unsigned char, SHA256_DIGEST_LENGTH> chunk;
        SHA256(buf.data(), n, chunk.data());
        chunks.push_back(chunk);
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
        return {CacheImportStatus::invalid, "local blob digest mismatch"};
    }
    // Keep normal read flags: O_CACHE_ONLY would also disable subsequent
    // remote cache-miss reads on a shared store. pwrite itself fills the cache.
    std::unique_ptr<photon::fs::IFile> cached(
        global_fs.cached_fs->open(url.c_str(), O_RDONLY));
    if (!cached || cached->ftruncate(size) != 0) {
        return {CacheImportStatus::unavailable, "cannot open managed cache entry"};
    }
    for (int64_t off = 0; off < size;) {
        const auto count = std::min<int64_t>(buf.size(), size - off);
        auto n = read_chunk(fd, buf.data(), count, off);
        if (n != count) {
            return {CacheImportStatus::invalid, "local blob changed during import"};
        }
        std::array<unsigned char, SHA256_DIGEST_LENGTH> chunk;
        SHA256(buf.data(), count, chunk.data());
        if (chunk != chunks[off / buf.size()]) {
            return {CacheImportStatus::invalid, "local blob changed during import"};
        }
        // Only these verified bytes are published, never an unchecked reread.
        if (cached->pwrite(buf.data(), count, off) != count) {
            return {CacheImportStatus::unavailable, "cache import incomplete; remote fallback remains available"};
        }
        off += count;
        photon::thread_yield();
    }
    return {CacheImportStatus::ok, ""};
}
