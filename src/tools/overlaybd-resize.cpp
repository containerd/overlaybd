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

#include <photon/common/alog.h>
#include <photon/photon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <string>
#include <sys/time.h>
#include "comm_func.h"
#include "CLI11.hpp"
#include "../image_file.h"

static uint64_t parse_size_gb(const std::string &s) {
    if (!s.empty() && s[0] == '-') {
        fprintf(stderr, "size must be positive: %s\n", s.c_str());
        return 0;
    }
    char *end = nullptr;
    errno = 0;
    uint64_t val = strtoull(s.c_str(), &end, 10);
    if (errno == ERANGE) {
        fprintf(stderr, "size value out of range: %s\n", s.c_str());
        return 0;
    }
    if (end == s.c_str() || *end != '\0') {
        fprintf(stderr, "invalid size (expected integer in GB): %s\n", s.c_str());
        return 0;
    }
    if (val == 0) {
        fprintf(stderr, "size must be greater than 0\n");
        return 0;
    }
    if (val > UINT64_MAX / (1024ULL * 1024 * 1024)) {
        fprintf(stderr, "size overflow: %s GB\n", s.c_str());
        return 0;
    }
    return val * 1024ULL * 1024 * 1024;
}

int main(int argc, char **argv) {
    std::string config_path, size_str, service_config_path;
    bool verbose = false;

    CLI::App app{"overlaybd-resize: resize ext4 filesystem on overlaybd image in userspace"};
    app.add_option("--config", config_path,
                   "overlaybd image config (config.v1.json)")
        ->type_name("FILEPATH")
        ->check(CLI::ExistingFile)
        ->required();
    app.add_option("--size", size_str,
                   "target filesystem size in GB (e.g. 8, 500)")
        ->required();
    app.add_option("--service_config_path", service_config_path,
                   "overlaybd service config")
        ->type_name("FILEPATH")
        ->default_val("/etc/overlaybd/overlaybd.json");
    app.add_flag("--verbose", verbose, "output debug info")->default_val(false);
    CLI11_PARSE(app, argc, argv);

    uint64_t target_size = parse_size_gb(size_str);
    if (target_size == 0) {
        fprintf(stderr, "invalid size: %s\n", size_str.c_str());
        return 1;
    }

    set_log_output_level(verbose ? 0 : 1);
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT) != 0) {
        fprintf(stderr, "failed to initialize photon\n");
        return 1;
    }
    DEFER({ photon::fini(); });

    ImageService *imgservice = create_image_service(service_config_path.c_str());
    if (!imgservice) {
        fprintf(stderr, "failed to create image service\n");
        return 1;
    }
    DEFER({ delete imgservice; });

    auto imgfile = (ImageFile *)imgservice->create_image_file(config_path.c_str(), "");
    if (!imgfile) {
        fprintf(stderr, "failed to open overlaybd image\n");
        return 1;
    }
    DEFER({ delete imgfile; });

    struct timeval t_start, t_end;
    gettimeofday(&t_start, NULL);

    int ret = imgfile->resize(target_size, true);
    if (ret != 0) {
        fprintf(stderr, "resize failed\n");
        return 1;
    }

    gettimeofday(&t_end, NULL);
    double elapsed = (t_end.tv_sec - t_start.tv_sec) + (t_end.tv_usec - t_start.tv_usec) / 1000000.0;

    printf("Success: filesystem resized to %llu GB (%.3f seconds)\n",
           (unsigned long long)(target_size / 1024 / 1024 / 1024), elapsed);
    return 0;
}
