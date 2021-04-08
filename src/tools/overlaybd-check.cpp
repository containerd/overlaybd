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
#include "../overlaybd/alog-stdstring.h"
#include "../overlaybd/alog.h"
#include "../overlaybd/fs/localfs.h"
#include "../overlaybd/fs/lsmt/file.h"
#include "../overlaybd/fs/registryfs/registryfs.h"
#include "../overlaybd/fs/zfile/tar_zfile.h"
#include "../overlaybd/net/curl.h"
#include "../overlaybd/photon/syncio/fd-events.h"
#include "../overlaybd/photon/thread.h"
#include "../overlaybd/utility.h"
#include "../config.h"
#include "../image_service.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;
using namespace FileSystem;

static void usage() {
    static const char msg[] =
        "overlaybd-check is a tool to check whether a remote blob is an overlaybd blob or not.\n"
        "Usage: overlaybd-check <url>\n"
        "example:\n"
        "   ./overlaybd-check https://docker.io/v2/overlaybd/imgxxx/blobs/sha256:xxxxx\n";

    puts(msg);
    exit(0);
}
string url, cred_path;
IFileSystem *registryfs;

static void parse_args(int &argc, char **argv) {
    int shift = 1;
    int ch;
    bool log = false;
    if (argc != 2)
        return usage();
    url = argv[shift];
}

std::pair<std::string, std::string> reload_registry_auth(void *, const char *remote_path) {
    LOG_INFO("Acquire credential for ", VALUE(remote_path));
    int retry = 0;
    std::string username, password;

    int res = load_cred_from_file(cred_path, std::string(remote_path), username, password);
    if (res == 0) {
        return std::make_pair(username, password);
    }
    printf("reload registry credential failed, token not found.\n");
    return std::make_pair("", "");
}

int main(int argc, char **argv) {
    set_log_output_level(4);
    parse_args(argc, argv);
    auto ret = photon::init();
    if (ret < 0) {
        printf("photon init failed.\n");
        return -1;
    }
    ret = photon::fd_events_init();
    if (ret < 0) {
        printf("photon fd_events_init failed.\n");
        return -1;
    }
    ret = Net::cURL::init();
    if (ret < 0) {
        printf("curl init failed.\n");
        return -1;
    }

    auto p = url.find("sha256:");
    if (p == string::npos) {
        printf("invalid blob url.\n");
        exit(-1);
    }

    ImageConfigNS::GlobalConfig obd_conf;
    if (!obd_conf.ParseJSON("/etc/overlaybd/config.json")) {
        printf("invalid overlaybd config file.\n");
        exit(-1);
    }
    cred_path = obd_conf.credentialFilePath();

    auto prefix = url.substr(0, p);
    auto suburl = url.substr(p);
    LOG_INFO("blob url: {prefix:`, file:`}", prefix, suburl);

    auto cafile = "/etc/ssl/certs/ca-bundle.crt";
    if (access(cafile, 0) != 0) {
        cafile = "/etc/ssl/certs/ca-certificates.crt";
        if (access(cafile, 0) != 0) {
            printf("no certificates found.");
            exit(-1);
        }
    }

    LOG_INFO("create registryfs with cafile:`", cafile);
    auto registryfs = FileSystem::new_registryfs_with_password_callback(
        "", {nullptr, &reload_registry_auth}, cafile, 36UL * 1000000);

    if (registryfs == nullptr) {
        printf("failed to create registryfs.\n");
        exit(-1);
    }
    auto *f = registryfs->open(url.c_str(), 0);
    if (f == nullptr) {
        printf("failed to open registry blob.\n");
        exit(-1);
    }

    if (FileSystem::is_tar_zfile(f) == 1)
        printf("true\n");
    else
        printf("false\n");

    delete f;
    return 0;
}
