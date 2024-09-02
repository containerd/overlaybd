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

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <memory>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../overlaybd/lsmt/file.h"
#include "../overlaybd/zfile/zfile.h"
#include "../image_service.h"
#include "../image_file.h"
#include <photon/common/alog.h>
#include <photon/common/uuid.h>
#include <photon/photon.h>
#include "CLI11.hpp"


int generate_option(CLI::App &app);
photon::fs::IFile *open_file(const char *fn, int flags, mode_t mode = 0, photon::fs::IFileSystem *fs = nullptr);

int create_overlaybd(const std::string &srv_config, const std::string &dev_config,
    ImageService *&, photon::fs::IFile *&);

photon::fs::IFile *create_uploader(ZFile::CompressArgs *zfile_args,  IFile *src,
    const std::string &upload_url, const std::string &cred_file_path, uint64_t timeout_minute, uint64_t upload_bs_KB,
    const std::string &tls_key_path,  const std::string &tls_cert_path);

photon::fs::IFileSystem *create_ext4fs(photon::fs::IFile *imgfile, bool mkfs,
    bool enable_buffer, const char* root);

bool is_erofs_fs(const photon::fs::IFile *imgfile);
photon::fs::IFileSystem *create_erofs_fs(photon::fs::IFile *imgfile, uint64_t blksz);
