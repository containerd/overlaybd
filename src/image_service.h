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

#include <string>
#include "config.h"
#include "exporter_server.h"
#include "overlaybd/cache/gzip_cache/cached_fs.h"
#include <photon/fs/filesystem.h>
#include <photon/common/io-alloc.h>

using namespace photon::fs;


struct GlobalFs {
    IFileSystem *underlay_registryfs = nullptr;
    IFileSystem *remote_fs = nullptr;
    IFileSystem *srcfs = nullptr;
    IFileSystem *cached_fs = nullptr;
    Cache::GzipCachedFs *gzcache_fs = nullptr;

    // ocf cache only
    IFile *media_file = nullptr;
    IFileSystem *namespace_fs = nullptr;
    IOAlloc *io_alloc = nullptr;
};

struct ImageAuthResponse : public ConfigUtils::Config {
    APPCFG_CLASS

    APPCFG_PARA(traceId, std::string, "");
	APPCFG_PARA(success, bool, false);
	APPCFG_PARA(data, ImageConfigNS::AuthConfig);
};

struct ImageFile;

class ImageService {
public:
    ImageService(const char *config_path = nullptr);
    ~ImageService();
    int init();
    ImageFile *create_image_file(const char *image_config_path);
    // bool enable_acceleration(GlobalFs *global_fs, ImageConfigNS::P2PConfig conf);
    bool enable_acceleration();


    ImageConfigNS::GlobalConfig global_conf;
    struct GlobalFs global_fs;
    std::unique_ptr<OverlayBDMetric> metrics;
    ExporterServer *exporter = nullptr;

private:
    int read_global_config_and_set();
    std::pair<std::string, std::string> reload_auth(const char *remote_path);
    void set_result_file(std::string &filename, std::string &data);
    std::string m_config_path;
};

ImageService *create_image_service(const char *config_path = nullptr);

int load_cred_from_file(const std::string path, const std::string &remote_path,
                        std::string &username, std::string &password);

void destroy();