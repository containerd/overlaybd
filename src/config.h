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
#include <vector>
#include "version.h"
#include "overlaybd/config_util.h"

namespace ImageConfigNS {
const int MAX_LAYER_CNT = 256;

struct LayerConfig : public ConfigUtils::Config {
    APPCFG_CLASS;

    APPCFG_PARA(gzipIndex, std::string, "");
    APPCFG_PARA(file, std::string, "");
    APPCFG_PARA(targetFile, std::string, "");
    APPCFG_PARA(dir, std::string, "");
    APPCFG_PARA(digest, std::string, "");
    APPCFG_PARA(targetDigest, std::string, "");
    APPCFG_PARA(size, uint64_t, 0);
};

struct UpperConfig : public ConfigUtils::Config {
    APPCFG_CLASS;

    APPCFG_PARA(index, std::string, "");
    APPCFG_PARA(data, std::string, "");
    APPCFG_PARA(target, std::string, "");
    APPCFG_PARA(gzipIndex, std::string, "");
};

struct DownloadConfig : public ConfigUtils::Config {
    APPCFG_CLASS;

    APPCFG_PARA(enable, bool, false);
    APPCFG_PARA(delay, int, 300);
    APPCFG_PARA(delayExtra, int, 30);
    APPCFG_PARA(maxMBps, int, 100);
    APPCFG_PARA(tryCnt, int, 5);
    APPCFG_PARA(blockSize, uint32_t, 262144);
};

struct ImageConfig : public ConfigUtils::Config {
    APPCFG_CLASS;

    APPCFG_PARA(repoBlobUrl, std::string, "");
    APPCFG_PARA(lowers, std::vector<LayerConfig>);
    APPCFG_PARA(upper, UpperConfig);
    APPCFG_PARA(resultFile, std::string, "");
    APPCFG_PARA(download, DownloadConfig);
    APPCFG_PARA(accelerationLayer, bool, false);
    APPCFG_PARA(recordTracePath, std::string, "");
};

struct P2PConfig : public ConfigUtils::Config {
    APPCFG_CLASS

    APPCFG_PARA(enable, bool, false);
    APPCFG_PARA(address, std::string, "http://localhost:9731/accelerator");
};

struct GzipCacheConfig : public ConfigUtils::Config {
    APPCFG_CLASS

    APPCFG_PARA(enable, bool, false);
    APPCFG_PARA(cacheDir, std::string, "/opt/overlaybd/gzip_cache");
    APPCFG_PARA(cacheSizeGB, uint32_t, 4);
    APPCFG_PARA(refillSize, uint32_t, 1024 * 1024);
};

struct ExporterConfig : public ConfigUtils::Config {
    APPCFG_CLASS

    APPCFG_PARA(enable, bool, false);
    APPCFG_PARA(uriPrefix, std::string, "/metrics");
    APPCFG_PARA(port, int, 9863);
    APPCFG_PARA(updateInterval, uint64_t, 60UL * 1000 * 1000);
};

struct CredentialConfig : public ConfigUtils::Config {
    APPCFG_CLASS
    APPCFG_PARA(mode, std::string, "");
    APPCFG_PARA(path, std::string, "");
    APPCFG_PARA(timeout, int, 1);
};

struct CacheConfig : public ConfigUtils::Config {
    APPCFG_CLASS

    APPCFG_PARA(cacheType, std::string, "");
    APPCFG_PARA(cacheDir, std::string, "/opt/overlaybd/registry_cache");
    APPCFG_PARA(cacheSizeGB, uint32_t, 4);
    APPCFG_PARA(refillSize, uint32_t, 262144);
    APPCFG_PARA(blockSize, uint32_t, 65536);
};

struct LogConfig : public ConfigUtils::Config {
    APPCFG_CLASS

    APPCFG_PARA(logLevel, uint32_t, 1);
    APPCFG_PARA(logPath, std::string, "");
    APPCFG_PARA(logSizeMB, uint32_t, 10);
    APPCFG_PARA(logRotateNum, int, 3);
};

struct PrefetchConfig : public ConfigUtils::Config {
    APPCFG_CLASS

    APPCFG_PARA(concurrency, int, 16);
};

struct CertConfig : public ConfigUtils::Config {
    APPCFG_CLASS

    APPCFG_PARA(certFile, std::string, "");
    APPCFG_PARA(keyFile, std::string, "");
};

struct GlobalConfig : public ConfigUtils::Config {
    APPCFG_CLASS

    APPCFG_PARA(registryCacheDir, std::string, "/opt/overlaybd/registry_cache");
    APPCFG_PARA(credentialFilePath, std::string, "/opt/overlaybd/cred.json");
    APPCFG_PARA(credentialConfig, CredentialConfig)
    APPCFG_PARA(registryCacheSizeGB, uint32_t, 4);
    APPCFG_PARA(ioEngine, uint32_t, 0);
    APPCFG_PARA(cacheType, std::string, "file");
    APPCFG_PARA(logLevel, uint32_t, 1);
    APPCFG_PARA(logPath, std::string, "/var/log/overlaybd.log");
    APPCFG_PARA(download, DownloadConfig);
    APPCFG_PARA(enableAudit, bool, true);
    APPCFG_PARA(enableThread, bool, false);
    APPCFG_PARA(p2pConfig, P2PConfig);
    APPCFG_PARA(exporterConfig, ExporterConfig);
    APPCFG_PARA(auditPath, std::string, "/var/log/overlaybd-audit.log");
    APPCFG_PARA(registryFsVersion, std::string, "v2");
    APPCFG_PARA(cacheConfig, CacheConfig);
    APPCFG_PARA(gzipCacheConfig, GzipCacheConfig);
    APPCFG_PARA(logConfig, LogConfig);
    APPCFG_PARA(prefetchConfig, PrefetchConfig);
    APPCFG_PARA(certConfig, CertConfig);
    APPCFG_PARA(userAgent, std::string, OVERLAYBD_VERSION);
};

struct AuthConfig : public ConfigUtils::Config {
    APPCFG_CLASS
    APPCFG_PARA(auths, ConfigUtils::Document);
};

struct ImageAuthResponse : public ConfigUtils::Config {
    APPCFG_CLASS

    APPCFG_PARA(traceId, std::string, "");
	APPCFG_PARA(success, bool, false);
	APPCFG_PARA(data, AuthConfig);
};

} // namespace ImageConfigNS
