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
#include "config.h"
#include "image_file.h"
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/io-alloc.h>
#include <photon/fs/localfs.h>
#include <photon/fs/path.h>
#include <photon/net/curl.h>
#include <photon/net/http/url.h>
#include <photon/net/socket.h>
#include <photon/thread/thread.h>
#include "overlaybd/cache/cache.h"
#include "overlaybd/registryfs/registryfs.h"
#include "overlaybd/zfile/zfile.h"
#include "overlaybd/base64.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

const char *DEFAULT_CONFIG_PATH = "/etc/overlaybd/overlaybd.json";
const int LOG_SIZE = 10 * 1024 * 1024;
const int LOG_NUM = 3;

struct ImageRef {
    std::vector<std::string> seg; // cr: seg_0, ns: seg_1, repo: seg_2, seg_3,...
};

bool create_dir(const char *dirname) {
    auto lfs = new_localfs_adaptor();
    if (lfs == nullptr) {
        LOG_ERRNO_RETURN(0, false, "new localfs_adaptor failed");
    }
    DEFER(delete lfs);
    if (lfs->access(dirname, 0) == 0) {
        return true;
    }
    if (lfs->mkdir(dirname, 0644) == 0) {
        LOG_INFO("dir ` doesn't exist. create succ.", dirname);
        return true;
    }
    LOG_ERRNO_RETURN(0, false, "dir ` doesn't exist. create failed.", dirname);
}

int parse_blob_url(const std::string &url, struct ImageRef &ref) {
    for (auto prefix : std::vector<std::string>{"http://", "https://"}) {
        auto p_colon = url.find(prefix);
        if (p_colon == 0) {
            auto sub_url = url.substr(prefix.length());
            std::vector<std::string> words;
            size_t idx = 0, prev = 0;
            while ((idx = sub_url.find("/", prev)) != std::string::npos) {
                LOG_DEBUG("find: `", sub_url.substr(prev, idx - prev));
                words.emplace_back(sub_url.substr(prev, idx - prev));
                prev = idx + 1;
            }
            ref.seg = std::vector<std::string>{words[0]};
            for (size_t i = 2; i + 1 < words.size(); i++) {
                ref.seg.push_back(words[i]);
            }
        }
    }
    return 0;
}

int parse_auths(const ConfigUtils::Document &auths, const std::string &remote_path,
    std::string &username, std::string &password) {

    struct ImageRef ref;
    if (parse_blob_url(remote_path, ref) != 0) {
        LOG_ERROR_RETURN(0, -1, "parse blob url failed: `", remote_path);
    }

    for (auto &iter : auths.GetObject()) {
        std::string addr = iter.name.GetString();
        LOG_DEBUG("cred addr: `", iter.name.GetString());

        std::string prefix = "";
        bool match = false;
        for (auto seg : ref.seg) {
            if (prefix != "")
                prefix += "/";
            prefix += seg;
            if (addr == prefix) {
                match = true;
                break;
            }
        }

        if (!match)
            continue;

        if (iter.value.HasMember("auth")) {
            auto token = base64_decode(iter.value["auth"].GetString());
            auto p = token.find(":");
            if (p == std::string::npos) {
                LOG_ERROR("invalid base64 auth, no ':' found: `", token);
                continue;
            }
            username = token.substr(0, p);
            password = token.substr(p + 1);
            return 0;
        } else if (iter.value.HasMember("username") &&
                   iter.value.HasMember("password")) {
            username = iter.value["username"].GetString();
            password = iter.value["password"].GetString();
            return 0;
        }
    }
    return 0;
}

int load_cred_from_file(const std::string path, const std::string &remote_path,
                        std::string &username, std::string &password) {
    ImageConfigNS::AuthConfig cfg;
    if (!cfg.ParseJSON(path)) {
        LOG_ERROR_RETURN(0, -1, "parse json failed: `", path);
    }
    return parse_auths(cfg.auths(), remote_path, username, password);
}

int load_cred_from_http(const std::string addr /* http server */, const std::string &remote_path,
                        std::string &username, std::string &password, int timeout) {

    auto request = new photon::net::cURL();
    DEFER({ delete request; });

    auto request_url = addr + "?remote_url=" + remote_path;
    LOG_INFO("request url: `", request_url);
    photon::net::StringWriter writer;
    auto ret = request->GET(request_url.c_str(), &writer, (int64_t)timeout * 1000000);
    if (ret != 200) {
        LOG_ERRNO_RETURN(0, -1, "connect to auth component failed. http response code: `", ret);
    }
    LOG_DEBUG(writer.string);
    ImageAuthResponse response;
    LOG_DEBUG("response size: `", writer.string.size());
    if (response.ParseJSONStream(writer.string) == false) {
        LOG_ERRNO_RETURN(0, -1, "parse http response message failed: `", writer.string);
    }
    LOG_INFO("traceId: `, succ: `", response.traceId(), response.success());
    if (response.success() == false) {
        LOG_ERRNO_RETURN(0, -1, "http request failed.");
    }
    ImageConfigNS::AuthConfig cfg;
    return parse_auths(response.data().auths(), remote_path, username, password);
}

int ImageService::read_global_config_and_set() {
    LOG_INFO("using config `", m_config_path);
    if (!global_conf.ParseJSON(m_config_path)) {
        LOG_ERROR_RETURN(0, -1, "error parse global config json: `", m_config_path);
    }
    uint32_t ioengine = global_conf.ioEngine();
    if (ioengine > 2) {
        LOG_ERROR_RETURN(0, -1, "unknown io_engine: `", ioengine);
    }

    if (global_conf.enableAudit()) {
        std::string auditPath = global_conf.auditPath();
        if (auditPath == "") {
            LOG_WARN("empty audit path, ignore audit");
        } else {
            LOG_INFO("set audit_path:`", global_conf.auditPath());
            default_audit_logger.log_output = new_log_output_file(global_conf.auditPath().c_str(), LOG_SIZE, LOG_NUM);
            if (!default_audit_logger.log_output) {
                default_audit_logger.log_output = log_output_null;
            }
        }
    } else {
        LOG_INFO("audit disabled");
    }

    uint32_t log_level, log_size, log_num;
    std::string log_path;
    LOG_INFO(VALUE(global_conf.logConfig().logPath()));
    if (global_conf.logConfig().logPath().empty()) {  // compatible for old config
        log_level = global_conf.logLevel();
        log_path = global_conf.logPath();
        log_size = LOG_SIZE;
        log_num = LOG_NUM;
    } else {
        log_level = global_conf.logConfig().logLevel();
        log_path = global_conf.logConfig().logPath();
        log_size = global_conf.logConfig().logSizeMB() * 1024 * 1024;
        log_num = global_conf.logConfig().logRotateNum();
    }

    set_log_output_level(log_level);
    LOG_INFO("set log_level: `", log_level);

    if (!log_path.empty()) {
        LOG_INFO("set log_path: `, log_size: `, log_num: `", log_path, log_size, log_num);
        int ret = log_output_file(log_path.c_str(), log_size, log_num);
        if (ret != 0) {
            LOG_ERROR_RETURN(0, -1, "log_output_file failed, errno:`", errno);
        }
    }
    // display in log file
    LOG_INFO("log config: ", VALUE(log_level), VALUE(log_path), VALUE(log_size), VALUE(log_num));
    return 0;
}

std::pair<std::string, std::string>
ImageService::reload_auth(const char *remote_path) {
    LOG_DEBUG("Acquire credential for ", VALUE(remote_path));
    std::string username, password;
    int res = 0;
    if (global_conf.credentialConfig().mode().empty()) {
        LOG_INFO("reload auth from legacy configuration [`]", global_conf.credentialFilePath());
        res = load_cred_from_file(global_conf.credentialFilePath(), std::string(remote_path), username, password);
    } else {
        auto mode = global_conf.credentialConfig().mode();
        auto path = global_conf.credentialConfig().path();
        if (path.empty()) {
            LOG_ERROR("empty authentication path.");
            return std::make_pair("","");
        }
        if (mode == "file") {
            res = load_cred_from_file(path, std::string(remote_path), username, password);
        } else if (mode == "http") {
            auto timeout = global_conf.credentialConfig().timeout();
            res = load_cred_from_http(path, std::string(remote_path), username, password, timeout);
        } else {
            LOG_ERROR("invalid mode for authentication.");
            return std::make_pair("","");
        }
    }
    if (res == 0) {
        LOG_INFO("auth found for `: `", remote_path, username);
        return std::make_pair(username, password);
    }
    return std::make_pair("", "");
}

void ImageService::set_result_file(std::string &filename, std::string &data) {
    if (filename == "") {
        LOG_WARN("no resultFile config set, ignore writing result");
        return;
    }

    auto file = open_localfile_adaptor(filename.c_str(),
                                                   O_RDWR | O_CREAT | O_TRUNC);
    if (file == nullptr) {
        LOG_ERROR("failed to open result file", filename);
        return;
    }
    DEFER(delete file);

    if (file->write(data.c_str(), data.size()) != (ssize_t)data.size()) {
        LOG_ERROR("write(`,`), path:`, `:`", data.c_str(), data.size(),
                  filename.c_str(), errno, strerror(errno));
    }
    LOG_DEBUG("write to result file: `, content: `", filename.c_str(),
              data.c_str());
}

size_t cache_fn_trans_sha256(void *, std::string_view origin, char *name, size_t namesize) {
    auto target = photon::fs::Path(origin).basename();
    if (target.size()+2 > namesize) {
        // return 0, no name trans, use origin name for cache
        LOG_ERROR_RETURN(ERANGE, 0, "name out of range");
    }
    name[0] = '/';
    strncpy(name + 1, target.data(), target.size());
    name[target.size()+1] = 0;
    return target.size()+1;
}

bool check_accelerate_url(std::string_view a_url) {
    photon::net::http::URL url(a_url);
    std::string host = url.host().data();
    auto pos = host.find(":");
    if (pos != host.npos) {
        host.resize(pos);
    }
    auto cli = photon::net::new_tcp_socket_client();
    DEFER({ delete cli; });
    auto sock = cli->connect({photon::net::IPAddr(host.c_str()), url.port()});
    if (sock == nullptr) {
        LOG_WARN("connect to accelerator failed: `", a_url);
        return false;
    }
    DEFER({ delete sock; });
    LOG_INFO("connect to accelerator success: `", a_url);
    return true;
}

int ImageService::init() {
    if (read_global_config_and_set() < 0) {
        return -1;
    }

    std::string cache_type, cache_dir;
    uint32_t cache_size_GB, refill_size, block_size;
    if (global_conf.cacheConfig().cacheType().empty()) {
        cache_type = global_conf.cacheType();
        cache_dir = global_conf.registryCacheDir();
        cache_size_GB = global_conf.registryCacheSizeGB();
    } else {
        cache_type = global_conf.cacheConfig().cacheType();
        cache_dir = global_conf.cacheConfig().cacheDir();
        cache_size_GB = global_conf.cacheConfig().cacheSizeGB();
    }
    refill_size = global_conf.cacheConfig().refillSize();
    block_size = global_conf.cacheConfig().blockSize();

    if (cache_type != "file" && cache_type != "ocf" && cache_type != "download") {
        LOG_ERROR_RETURN(0, -1, "unknown cache type: `", cache_type);
    }
    LOG_INFO("cache config: ", VALUE(cache_type), VALUE(cache_dir),
                               VALUE(cache_size_GB), VALUE(refill_size));

    if (!create_dir(cache_dir.c_str()))
        return -1;

    if (global_fs.remote_fs == nullptr) {
        auto cafile = "/etc/ssl/certs/ca-bundle.crt";
        if (access(cafile, 0) != 0) {
            cafile = "/etc/ssl/certs/ca-certificates.crt";
            if (access(cafile, 0) != 0) {
                LOG_ERROR_RETURN(0, -1, "no certificates found.");
            }
        }

        LOG_INFO("create registryfs with cafile:`, version:`", cafile, global_conf.registryFsVersion());
        auto registryfs_creator = new_registryfs_v1;
        if (global_conf.registryFsVersion() == "v2")
            registryfs_creator = new_registryfs_v2;

        global_fs.underlay_registryfs = registryfs_creator(
            {this, &ImageService::reload_auth}, cafile, 30UL * 1000000,
            global_conf.certConfig().certFile().c_str(), global_conf.certConfig().keyFile().c_str(), global_conf.userAgent().c_str());
        if (global_fs.underlay_registryfs == nullptr) {
            LOG_ERROR_RETURN(0, -1, "create registryfs failed.");
        }
        if (global_conf.exporterConfig().enable()) {
            metrics.reset(new OverlayBDMetric());
            global_fs.srcfs = new MetricFS(global_fs.underlay_registryfs, &metrics->download);
            exporter = new ExporterServer(global_conf, metrics.get());
            if (!exporter->ready)
                LOG_ERROR_RETURN(0, -1, "Failed to start http server for metrics exporter");
        } else {
            global_fs.srcfs = global_fs.underlay_registryfs;
        }

        if (global_conf.enableThread() == true && cache_type == "file") {
            LOG_ERROR_RETURN(0, -1, "multi-thread has not been valid for file cache");
        }

        global_fs.io_alloc = new IOAlloc;

        if (cache_type == "file") {
            auto registry_cache_fs = new_localfs_adaptor(cache_dir.c_str());
            if (registry_cache_fs == nullptr) {
                delete global_fs.srcfs;
                LOG_ERROR_RETURN(0, -1, "new_localfs_adaptor for ` failed", cache_dir.c_str());
            }
            // file cache will delete its src_fs automatically when destructed
            global_fs.cached_fs = FileSystem::new_full_file_cached_fs(
                global_fs.srcfs, registry_cache_fs, refill_size, cache_size_GB, 10000000,
                (uint64_t)1048576 * 1024, global_fs.io_alloc, 0, {nullptr, &cache_fn_trans_sha256});

        } else if (cache_type == "ocf") {
            auto namespace_dir = std::string(cache_dir + "/namespace");
            if (::access(namespace_dir.c_str(), F_OK) != 0 && ::mkdir(namespace_dir.c_str(), 0755) != 0) {
                LOG_ERRNO_RETURN(0, -1, "failed to create namespace_dir");
            }
            auto namespace_fs = new_localfs_adaptor(namespace_dir.c_str());
            if (namespace_fs == nullptr) {
                LOG_ERROR_RETURN(0, -1, "failed tp create namespace_fs");
            }
            global_fs.namespace_fs = namespace_fs;

            bool reload_media;
            IFile* media_file;
            auto media_file_path = std::string(cache_dir + "/cache_media");
            if (::access(media_file_path.c_str(), F_OK) != 0) {
                reload_media = false;
                media_file = open_localfile_adaptor(media_file_path.c_str(), O_RDWR | O_CREAT, 0644,
                                                                ioengine_psync);
                media_file->fallocate(0, 0, cache_size_GB * 1024UL * 1024 * 1024);
            } else {
                reload_media = true;
                media_file = open_localfile_adaptor(media_file_path.c_str(), O_RDWR, 0644,
                                                                ioengine_psync);
            }
            global_fs.media_file = media_file;

            global_fs.cached_fs = FileSystem::new_ocf_cached_fs(global_fs.srcfs, namespace_fs, block_size, refill_size,
                                                                media_file, reload_media, global_fs.io_alloc);
        } else if (cache_type == "download") {
            global_fs.cached_fs = FileSystem::new_download_cached_fs(global_fs.srcfs, 4096, refill_size, global_fs.io_alloc);
        } else {
            LOG_ERROR_RETURN(0, -1, "cache type invalid");
        }
        if (global_fs.cached_fs == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "failed to create cached_fs");
        }

        if (global_conf.exporterConfig().enable()) {
            global_fs.cached_fs = new MetricFS(global_fs.cached_fs, &metrics->pread);
        }

        if (global_conf.gzipCacheConfig().enable()) {
            LOG_INFO("use gzip file cache");
            cache_dir = global_conf.gzipCacheConfig().cacheDir();
            cache_size_GB = global_conf.gzipCacheConfig().cacheSizeGB();
            refill_size = global_conf.gzipCacheConfig().refillSize();
            if (!create_dir(cache_dir.c_str())) {
                return -1;
            }
            auto gzip_cache_fs = new_localfs_adaptor(cache_dir.c_str());
            if (gzip_cache_fs == nullptr) {
                delete global_fs.srcfs;
                LOG_ERROR_RETURN(0, -1, "new_localfs_adaptor for ` failed", cache_dir.c_str());
            }

            global_fs.gzcache_fs = Cache::new_gzip_cached_fs(
                gzip_cache_fs, refill_size, cache_size_GB,
                10000000, (uint64_t)1048576 * 4096, global_fs.io_alloc);
        }
    }
    return 0;
}

bool ImageService::enable_acceleration() {
    auto conf = global_conf.p2pConfig();
    if (conf.enable() && check_accelerate_url(conf.address())) {
        ((RegistryFS*)global_fs.underlay_registryfs)->setAccelerateAddress(conf.address().c_str());
        global_fs.remote_fs = global_fs.srcfs;
        return true;
    } else {
        ((RegistryFS*)(global_fs.underlay_registryfs))->setAccelerateAddress();
        global_fs.remote_fs = global_fs.cached_fs;
        return false;
    }
}

ImageFile *ImageService::create_image_file(const char *config_path) {
    ImageConfigNS::GlobalConfig defaultDlCfg;
    if (!defaultDlCfg.ParseJSON(m_config_path)) {
        LOG_WARN("default download config parse failed, ignore");
    }
    ImageConfigNS::ImageConfig cfg;
    if (!cfg.ParseJSON(config_path)) {
        LOG_ERROR_RETURN(0, nullptr, "error parse image config");
    }

    if (!cfg.HasMember("download") && !defaultDlCfg.IsNull() &&
        defaultDlCfg.HasMember("download")) {
        cfg.AddMember("download", defaultDlCfg["download"], cfg.GetAllocator());
    }

    if (enable_acceleration()) {
        LOG_INFO("use p2p proxy for acceleration, proxy: `",
            global_conf.p2pConfig().address());
    } else {
        LOG_INFO("use cache");
    }

    auto resFile = cfg.resultFile();
    ImageFile *ret = new ImageFile(cfg, *this);
    if (ret->m_status <= 0) {
        std::string data = "failed:" + ret->m_exception;
        set_result_file(resFile, data);
        delete ret;
        return NULL;
    }
    std::string data = "success";
    set_result_file(resFile, data);
    return ret;
}

ImageService::ImageService(const char *config_path) {
    m_config_path = config_path ? config_path : DEFAULT_CONFIG_PATH;
}

ImageService::~ImageService() {
    delete global_fs.media_file;
    delete global_fs.namespace_fs;
    delete global_fs.cached_fs;
    delete global_fs.gzcache_fs;
    delete global_fs.srcfs;
    delete global_fs.io_alloc;
    delete exporter;
    LOG_INFO("image service is fully stopped");
}

ImageService *create_image_service(const char *config_path) {
    ImageService *ret = new ImageService(config_path);
    if (ret->init() < 0) {
        delete ret;
        return nullptr;
    }
    return ret;
}
