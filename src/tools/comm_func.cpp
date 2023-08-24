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

#include "comm_func.h"
#include "../overlaybd/tar/tar_file.h"
#include <openssl/sha.h>
#include <photon/fs/subfs.h>

#include "../overlaybd/extfs/extfs.h"
#include "../image_service.h"
#include "../image_file.h"


using namespace std;
using namespace photon::fs;

IFile *open_file(const char *fn, int flags, mode_t mode, IFileSystem *fs) {
    IFile *file = nullptr;
    if (fs) {
        file = fs->open(fn, flags, mode);
    } else {
        file = open_localfile_adaptor(fn, flags, mode, 0);
    }
    if (!file) {
        fprintf(stderr, "failed to open file '%s', %d: %s\n", fn, errno, strerror(errno));
        exit(-1);
    }
    return file;
}

int create_overlaybd(const std::string &srv_config, const std::string &dev_config,
    ImageService *&imgservice, photon::fs::IFile *&imgfile) {

    imgservice = create_image_service(srv_config.c_str());
    if (imgservice == nullptr) {
        fprintf(stderr, "failed to create image service\n");
        exit(-1);
    }
    imgfile = imgservice->create_image_file(dev_config.c_str());
    if (imgfile == nullptr) {
        fprintf(stderr, "failed to create image file\n");
        exit(-1);
    }
    return 0;
};

photon::fs::IFileSystem *create_ext4fs(photon::fs::IFile *imgfile, bool mkfs,
    bool enable_buffer, const char* root){
    if (mkfs) {
        if (make_extfs(imgfile) < 0) {
            fprintf(stderr, "mkfs failed, %s\n", strerror(errno));
            exit(-1);
        }
    }
    // for now, buffer_file can't be used with fastoci
    auto extfs = new_extfs(imgfile, enable_buffer);
    if (!extfs) {
        fprintf(stderr, "new extfs failed, %s\n", strerror(errno));
        exit(-1);
    }
    auto target = new_subfs(extfs, root, true);
    if (!target) {
        fprintf(stderr, "new subfs failed, %s\n", strerror(errno));
        exit(-1);
    }
    return target;
}
