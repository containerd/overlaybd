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

// Real ImageService + Photon file cache + loopback HTTP origin/accelerator.
// No external registry, ublk device, or privileged kernel operation is needed.
#include "../image_service.h"
#include <photon/fs/cache/cache.h>
#include <photon/net/http/server.h>
#include <photon/net/socket.h>
#include <photon/photon.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <ftw.h>
#include <iostream>
#include <memory>
#include <unistd.h>

using namespace photon::net;
using namespace photon::net::http;

class BlobServer {
public:
    const std::string data = std::string(768 * 1024 + 17, 'x');
    std::string digest, url, last_target;
    int requests = 0;
    ISocketServer *socket = new_tcp_socket_server();
    HTTPServer *http = new_http_server();

    explicit BlobServer(bool listen = true) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256((const unsigned char *)data.data(), data.size(), hash);
        digest = "sha256:";
        for (auto byte : hash) {
            digest += "0123456789abcdef"[byte >> 4];
            digest += "0123456789abcdef"[byte & 15];
        }
        assert(socket && http);
        assert(socket->bind(0, IPAddr("127.0.0.1")) == 0);
        url = "http://127.0.0.1:" + std::to_string(socket->getsockname().port);
        http->add_handler({this, &BlobServer::handle});
        if (listen) {
            assert(socket->listen() == 0);
            socket->set_handler(http->get_connection_handler());
            socket->start_loop();
        }
    }

    ~BlobServer() {
        socket->terminate();
        delete socket;
        delete http;
    }

    int handle(Request &req, Response &resp, std::string_view) {
        ++requests;
        last_target = std::string(req.target());
        assert(last_target.find("/blobs/" + digest) != std::string::npos);
        size_t begin = 0, end = data.size() - 1;
        auto range = std::string(req.headers["Range"]);
        if (!range.empty()) {
            assert(std::sscanf(range.c_str(), "bytes=%zu-%zu", &begin, &end) == 2);
            end = std::min(end, data.size() - 1);
            assert(begin <= end);
            resp.set_result(206);
            resp.headers.insert("Content-Range", "bytes " + std::to_string(begin) + "-" +
                                std::to_string(end) + "/" + std::to_string(data.size()));
        } else {
            resp.set_result(200);
        }
        resp.headers.content_length(end - begin + 1);
        if (req.verb() != Verb::HEAD)
            assert(resp.write(data.data() + begin, end - begin + 1) == (ssize_t)(end - begin + 1));
        return 0;
    }
};

static void read_range(photon::fs::IFile *file, const BlobServer &blob,
                       size_t offset, size_t count) {
    std::string bytes(count, '\0');
    assert(file->pread(&bytes[0], count, offset) == (ssize_t)count);
    assert(bytes == blob.data.substr(offset, count));
}

static void test_routing(const std::string &root, const std::string &version,
                         bool cache_first, bool available, bool p2p_enabled = true) {
    BlobServer origin, accelerator(available);
    const auto blob_url = origin.url + "/v2/repo/blobs/" + origin.digest;
    const auto config_path = root + "/config.json";
    const auto staging = root + "/blob";
    std::ofstream(staging, std::ios::binary) << origin.data;
    // Exercise both configuration forms through actual initialization.
    const auto cache_config = version == "v1"
        ? "\"cacheType\":\"file\",\"registryCacheDir\":\"" + root + "/cache\",\"registryCacheSizeGB\":1,"
        : "\"cacheType\":\"ocf\",\"cacheConfig\":{\"cacheType\":\"file\",\"cacheDir\":\"" + root +
          "/cache\",\"cacheSizeGB\":1},";
    std::ofstream(config_path)
        << "{\"enableAudit\":false,\"logPath\":\"\",\"logLevel\":3,"
        << "\"registryFsVersion\":\"" << version << "\","
        << cache_config << "\"p2pConfig\":{\"enable\":"
        << (p2p_enabled ? "true" : "false") << ",\"cacheFirst\":"
        << (cache_first ? "true" : "false") << ",\"address\":\""
        << accelerator.url << "/accelerator\"}}";
    ImageService service(config_path.c_str());
    assert(service.init() == 0);
    assert(service.global_fs.remote_fs == nullptr);

    auto import = [&] {
        return service.import_local_blob(staging, blob_url, origin.digest,
                                         origin.data.size()).status;
    };
    const bool import_allowed = !p2p_enabled || cache_first;
    // Import before the first image selects its read path.
    assert(import() == (import_allowed ? CacheImportStatus::ok : CacheImportStatus::unsupported));
    assert(origin.requests == 0 && accelerator.requests == 0);
    assert(service.enable_acceleration() == (p2p_enabled && available));
    if (cache_first || !p2p_enabled || !available)
        assert(service.global_fs.remote_fs == service.global_fs.cached_fs);
    else
        assert(service.global_fs.remote_fs == service.global_fs.srcfs);
    assert(import() == (import_allowed ? CacheImportStatus::ok : CacheImportStatus::unsupported));
    assert(::unlink(staging.c_str()) == 0);

    std::unique_ptr<photon::fs::IFile> file(
        service.global_fs.remote_fs->open(blob_url.c_str(), O_RDONLY));
    assert(file);
    read_range(file.get(), origin, 0, origin.data.size());

    if (import_allowed) {
        // Imported bytes, including the unaligned tail, need no HTTP request.
        assert(origin.requests == 0 && accelerator.requests == 0);
        // Re-selection (as done by subsequent image creation) retains the cache.
        assert(service.enable_acceleration() == (p2p_enabled && available));
        assert(service.global_fs.remote_fs == service.global_fs.cached_fs);
        auto cached = dynamic_cast<photon::fs::ICachedFile *>(file.get());
        assert(cached && cached->get_store());
        // Evict one aligned range; untouched ranges must still hit locally.
        const size_t block = 256 * 1024;
        assert(cached->get_store()->evict(block, block) == 0);
        read_range(file.get(), origin, 0, block);
        read_range(file.get(), origin, 2 * block, origin.data.size() - 2 * block);
        assert(origin.requests == 0 && accelerator.requests == 0);
        read_range(file.get(), origin, block, block);
        if (p2p_enabled && available) {
            assert(accelerator.requests > 0);
            assert(accelerator.last_target.find("/accelerator/" + origin.url) == 0);
            // RegistryFS v1 may obtain origin metadata before using P2P.
            if (version == "v2") assert(origin.requests == 0);
        } else {
            assert(origin.requests > 0 && accelerator.requests == 0);
        }
        int origin_requests = origin.requests, p2p_requests = accelerator.requests;
        read_range(file.get(), origin, 0, origin.data.size());
        assert(origin.requests == origin_requests && accelerator.requests == p2p_requests);
        // A full eviction remains refillable, and refilled bytes hit thereafter.
        assert(cached->get_store()->evict(0, origin.data.size()) == 0);
        read_range(file.get(), origin, 0, origin.data.size());
        assert(origin.requests + accelerator.requests > origin_requests + p2p_requests);
        origin_requests = origin.requests;
        p2p_requests = accelerator.requests;
        read_range(file.get(), origin, 0, origin.data.size());
        assert(origin.requests == origin_requests && accelerator.requests == p2p_requests);
    } else if (available) {
        // Default P2P behavior still bypasses the managed file cache.
        int requests = accelerator.requests;
        assert(requests > 0);
        read_range(file.get(), origin, 0, origin.data.size());
        assert(accelerator.requests > requests);
    }
    std::cout << "PASS registry=" << version << " cacheFirst=" << cache_first
              << " p2pEnabled=" << p2p_enabled << " available=" << available << std::endl;
}

static int remove_entry(const char *path, const struct stat *, int, struct FTW *) {
    return ::remove(path);
}

int main() {
    assert(photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_DEFAULT) == 0);
    struct Mode { bool cache_first, available, enabled; };
    const Mode modes[] = {
        {true, true, true},   // Cache first with an available accelerator.
        {false, true, true},  // Existing cache bypass.
        {true, false, true},  // Cache first falls back to the origin.
        {false, false, true}, // Bypass policy still rejects early imports.
        {true, true, false},  // P2P disabled.
    };
    for (const auto &version : {"v2", "v1"}) {
        for (const auto &mode : modes) {
            char root[] = "/tmp/p2p-cache-test-XXXXXX";
            assert(mkdtemp(root));
            test_routing(root, version, mode.cache_first, mode.available, mode.enabled);
            assert(::nftw(root, remove_entry, 32, FTW_DEPTH | FTW_PHYS) == 0);
        }
    }
    for (const auto &type : {"ocf", "download"}) {
        char root[] = "/tmp/p2p-cache-config-XXXXXX";
        assert(mkdtemp(root));
        auto path = std::string(root) + "/config.json";
        std::ofstream(path) << "{\"enableAudit\":false,\"logPath\":\"\",\"cacheType\":\""
                            << type << "\",\"p2pConfig\":{\"cacheFirst\":true}}";
        { ImageService service(path.c_str()); assert(service.init() != 0); }
        assert(::nftw(root, remove_entry, 32, FTW_DEPTH | FTW_PHYS) == 0);
    }
    photon::fini();
}
