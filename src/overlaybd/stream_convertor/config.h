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
#include "config_utils.h"

namespace App {

struct LogConfigPara : public App::ConfigGroup {
    APPCFG_CLASS
    APPCFG_PARA(level, uint32_t, 1);
    APPCFG_PARA(path, std::string, "/var/log/overlaybd/stream-convertor.log");
    APPCFG_PARA(limitSizeMB, uint32_t, 10);
    APPCFG_PARA(rotateNum, int, 3);
    APPCFG_PARA(mode, std::string, "stdout");
};

struct GlobalConfigPara : public App::ConfigGroup  {
    APPCFG_CLASS;
    APPCFG_PARA(udsAddr, std::string, "");
    APPCFG_PARA(httpAddr, std::string, "127.0.0.1");
    APPCFG_PARA(httpPort, int, 9101);
    APPCFG_PARA(reusePort, bool, true);

    APPCFG_PARA(workDir, std::string, "/tmp/stream_conv");
    //APPCFG_PARA(ServerConfig, ServerConfigPara);
    APPCFG_PARA(logConfig, LogConfigPara);
};

struct AppConfig : public App::ConfigGroup {
    APPCFG_CLASS

    APPCFG_PARA(globalConfig, GlobalConfigPara);
};

} // namespace ImageConfigNS
