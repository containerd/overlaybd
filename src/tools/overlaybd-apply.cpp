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
#include <photon/fs/localfs.h>
#include <photon/fs/subfs.h>
#include <photon/photon.h>
#include "../overlaybd/lsmt/file.h"
#include "../overlaybd/zfile/zfile.h"
#include "../overlaybd/untar/libtar.h"
#include "../overlaybd/extfs/extfs.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <memory>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "../image_service.h"
#include "../image_file.h"
#include "CLI11.hpp"

using namespace std;
using namespace photon::fs;

IFile *open_file(const char *fn, int flags, mode_t mode = 0) {
    auto file = open_localfile_adaptor(fn, flags, mode, 0);
    if (!file) {
        fprintf(stderr, "failed to open file '%s', %d: %s\n", fn, errno, strerror(errno));
        exit(-1);
    }
    return file;
}

int main(int argc, char **argv) {
    string commit_msg;
    string parent_uuid;
    std::string image_config_path, input_path;
    bool compress_zfile = false;

    CLI::App app{"this is overlaybd-apply, apply OCIv1 tar layer to overlaybd format"};
    app.add_option("input_path", input_path, "input OCIv1 tar layer path")->type_name("FILEPATH")->check(CLI::ExistingFile)->required();
    app.add_option("image_config_path", image_config_path, "overlaybd image config path")->type_name("FILEPATH")->check(CLI::ExistingFile)->required();
    CLI11_PARSE(app, argc, argv);

    set_log_output_level(1);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);

    auto imgservice = create_image_service();
    ImageFile *imgfile = imgservice->create_image_file(image_config_path.c_str());
    if (imgfile == nullptr) {
        fprintf(stderr, "failed to create image file\n");
        exit(-1);
    }
    auto extfs = new_extfs(imgfile);
    if (!extfs) {
        fprintf(stderr, "new extfs failed, %s\n", strerror(errno));
        exit(-1);
    }
    auto target = new_subfs(extfs, "/", true);
    if (!target) {
        fprintf(stderr, "new subfs failed, %s\n", strerror(errno));
        exit(-1);
    }

    auto tarf = open_file(input_path.c_str(), O_RDONLY, 0666);
    auto tar = new Tar(tarf, target, 0);
    if (tar->extract_all() < 0) {
        fprintf(stderr, "failed to extract\n");
        exit(-1);
    } else {
        fprintf(stderr, "overlaybd-apply done\n");
    }

    delete target;
    delete imgfile;

    return 0;
}
