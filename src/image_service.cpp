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
#include "overlaybd/alog-stdstring.h"
#include "overlaybd/alog.h"
#include "overlaybd/base64.h"
#include "overlaybd/fs/cache/cache.h"
#include "overlaybd/fs/filesystem.h"
#include "overlaybd/fs/localfs.h"
#include "overlaybd/fs/registryfs/registryfs.h"
#include "overlaybd/fs/zfile/tar_zfile.h"
#include "overlaybd/fs/zfile/zfile.h"
#include "overlaybd/photon/thread.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

const char *DEFAULT_CONFIG_PATH = "/etc/overlaybd/overlaybd.json";

struct ImageRef {
    std::vector<std::string> seg; // cr: seg_0, ns: seg_1, repo: seg_2
};

bool create_dir(const char *dirname) {
    auto lfs = FileSystem::new_localfs_adaptor();
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
            if (words.size() != 5) {
                LOG_ERROR_RETURN(0, -1, "invalid blob url: `", url);
            }
            ref.seg = std::vector<std::string>{words[0], words[2], words[3]};
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

    struct ImageRef ref;
    if (parse_blob_url(remote_path, ref) != 0) {
        LOG_ERROR_RETURN(0, -1, "parse blob url failed: `", remote_path);
    }

    for (auto &iter : cfg.auths().GetObject()) {
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

    return -1;
}

int ImageService::read_global_config_and_set() {
    if (!global_conf.ParseJSON(DEFAULT_CONFIG_PATH)) {
        LOG_ERROR_RETURN(0, -1, "error parse global config json: `",
                         DEFAULT_CONFIG_PATH);
    }
    uint32_t ioengine = global_conf.ioEngine();
    if (ioengine > 2) {
        LOG_ERROR_RETURN(0, -1, "unknown io_engine: `", ioengine);
    }

    LOG_INFO("global config: cache_dir: `, cache_size_GB: `",
             global_conf.registryCacheDir(), global_conf.registryCacheSizeGB());
    set_log_output_level(global_conf.logLevel());
    LOG_INFO("set log_level:`", global_conf.logLevel());

    if (global_conf.logPath() != "") {
        LOG_INFO("set log_path:`", global_conf.logPath());
        int ret = log_output_file(global_conf.logPath().c_str(),
                                  100 * 1024 * 1024, 5);
        if (ret != 0) {
            LOG_ERROR_RETURN(0, -1, "log_output_file failed, errno:`", errno);
        }
    }
    return 0;
}

std::pair<std::string, std::string>
ImageService::reload_auth(const char *remote_path) {
    LOG_DEBUG("Acquire credential for ", VALUE(remote_path));
    std::string username, password;

    int res = load_cred_from_file(global_conf.credentialFilePath(), std::string(remote_path), username, password);
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

    auto file = FileSystem::open_localfile_adaptor(filename.c_str(),
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

int ImageService::init() {
    if (read_global_config_and_set() < 0) {
        return -1;
    }

    if (create_dir(global_conf.registryCacheDir().c_str()) == false)
        return -1;

    if (global_fs.remote_fs == nullptr) {
        auto cafile = "/etc/ssl/certs/ca-bundle.crt";
        if (access(cafile, 0) != 0) {
            cafile = "/etc/ssl/certs/ca-certificates.crt";
            if (access(cafile, 0) != 0) {
                LOG_ERROR_RETURN(0, -1, "no certificates found.");
            }
        }

        LOG_INFO("create registryfs with cafile:`", cafile);
        auto registry_fs = FileSystem::new_registryfs_with_credential_callback(
            {this, &ImageService::reload_auth}, cafile, 30UL * 1000000);
        if (registry_fs == nullptr) {
            LOG_ERROR_RETURN(0, -1, "create registryfs failed.");
        }

        auto zfile_fs = FileSystem::new_tar_zfile_fs_adaptor(registry_fs);
        if (zfile_fs == nullptr) {
            delete registry_fs;
            LOG_ERROR_RETURN(0, -1, "create zfile_fs failed.");
        }

        auto registry_cache_fs = FileSystem::new_localfs_adaptor(
            global_conf.registryCacheDir().c_str());
        if (registry_cache_fs == nullptr) {
            delete zfile_fs;
            LOG_ERROR_RETURN(0, -1, "new_localfs_adaptor for ` failed",
                             global_conf.registryCacheDir().c_str());
            return false;
        }

        LOG_INFO("create cache with size: ` GB",
                 global_conf.registryCacheSizeGB());
        global_fs.remote_fs = FileSystem::new_full_file_cached_fs(
            zfile_fs, registry_cache_fs, 256 * 1024 /* refill unit 256KB */,
            global_conf.registryCacheSizeGB() /*GB*/, 10000000,
            (uint64_t)1048576 * 4096, nullptr);

        if (global_fs.remote_fs == nullptr) {
            delete zfile_fs;
            delete registry_cache_fs;
            LOG_ERROR_RETURN(0, -1,
                             "create remotefs (registryfs + cache) failed.");
        }
        global_fs.cachefs = registry_cache_fs;
        global_fs.srcfs = zfile_fs;
    }
    return 0;
}

ImageFile *ImageService::create_image_file(const char *config_path) {
    ImageConfigNS::GlobalConfig defaultDlCfg;
    if (!defaultDlCfg.ParseJSON(DEFAULT_CONFIG_PATH)) {
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

    auto resFile = cfg.resultFile();
    ImageFile *ret = new ImageFile(cfg, global_fs, global_conf);
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

ImageService *create_image_service() {
    ImageService *ret = new ImageService();
    if (ret->init() < 0) {
        delete ret;
        return nullptr;
    }
    return ret;
}