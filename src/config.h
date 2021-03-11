/*
 * config.h
 *
 * Copyright (C) 2021 Alibaba Group.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */

#pragma once

#include <string>
#include <vector>
#include "overlaybd/config_util.h"

namespace ImageConfigNS {
const int MAX_LAYER_CNT = 256;

struct LayerConfig : public ConfigUtils::Config {
    APPCFG_CLASS;

    APPCFG_PARA(file, std::string, "");
    APPCFG_PARA(dir, std::string, "");
    APPCFG_PARA(digest, std::string, "");
    APPCFG_PARA(size, uint64_t, 0);
};

struct UpperConfig : public ConfigUtils::Config {
    APPCFG_CLASS;

    APPCFG_PARA(index, std::string, "");
    APPCFG_PARA(data, std::string, "");
};

struct DownloadConfig : public ConfigUtils::Config {
    APPCFG_CLASS;

    APPCFG_PARA(enable, bool, false);
    APPCFG_PARA(delay, int, 300);
    APPCFG_PARA(delayExtra, int, 30);
    APPCFG_PARA(maxMBps, int, 100);
    APPCFG_PARA(tryCnt, int, 5);
};

struct ImageConfig : public ConfigUtils::Config {
    APPCFG_CLASS;

    APPCFG_PARA(repoBlobUrl, std::string, "");
    APPCFG_PARA(lowers, std::vector<LayerConfig>);
    APPCFG_PARA(upper, UpperConfig);
    APPCFG_PARA(resultFile, std::string, "");
    APPCFG_PARA(download, DownloadConfig);
};

struct GlobalConfig : public ConfigUtils::Config {
    APPCFG_CLASS

    APPCFG_PARA(registryCacheDir, std::string, "/opt/overlaybd/registryfs_cache");
    APPCFG_PARA(credentialFilePath, std::string, "/opt/overlaybd/cred.json");
    APPCFG_PARA(registryCacheSizeGB, uint32_t, 4);
    APPCFG_PARA(ioEngine, uint32_t, 0);
    APPCFG_PARA(logLevel, uint32_t, 1);
    APPCFG_PARA(download, DownloadConfig);
};

struct AuthConfig : public ConfigUtils::Config {
    APPCFG_CLASS

    APPCFG_PARA(auths, ConfigUtils::Document);
};

} // namespace ImageConfigNS