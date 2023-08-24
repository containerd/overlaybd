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
#include <photon/fs/localfs.h>
#include <photon/common/uuid.h>
#include <photon/photon.h>
#include "CLI11.hpp"
#include <openssl/sha.h>


int generate_option(CLI::App &app);
photon::fs::IFile *open_file(const char *fn, int flags, mode_t mode = 0, photon::fs::IFileSystem *fs = nullptr);

int create_overlaybd(const std::string &srv_config, const std::string &dev_config,
    ImageService *&, photon::fs::IFile *&);

photon::fs::IFileSystem *create_ext4fs(photon::fs::IFile *imgfile, bool mkfs,
    bool enable_buffer, const char* root);

class SHA256CheckedFile: public VirtualReadOnlyFile {
public:
    IFile *m_file;
    SHA256_CTX ctx = {0};
    size_t total_read = 0;

    SHA256CheckedFile(IFile *file): m_file(file) {
        SHA256_Init(&ctx);
    }
    ~SHA256CheckedFile() {
        delete m_file;
    }
    virtual IFileSystem *filesystem() override {
        return nullptr;
    }
    ssize_t read(void *buf, size_t count) override {
        auto rc = m_file->read(buf, count);
        if (rc > 0 && SHA256_Update(&ctx, buf, rc) < 0) {
            LOG_ERROR("sha256 calculate error");
            return -1;
        }
        return rc;
    }
    off_t lseek(off_t offset, int whence) override {
        return m_file->lseek(offset, whence);
    }
    std::string sha256_checksum() {
        // read trailing data
        char buf[64*1024];
        auto rc = m_file->read(buf, 64*1024);
        if (rc == 64*1024) {
            LOG_WARN("too much trailing data");
        }
        if (rc > 0 && SHA256_Update(&ctx, buf, rc) < 0) {
            LOG_ERROR("sha256 calculate error");
            return "";
        }
        // calc sha256 result
        unsigned char sha[32];
        SHA256_Final(sha, &ctx);
        char res[SHA256_DIGEST_LENGTH * 2];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            sprintf(res + (i * 2), "%02x", sha[i]);
        return "sha256:" + std::string(res, SHA256_DIGEST_LENGTH * 2);
    }
    int fstat(struct stat *buf) override {
        return m_file->fstat(buf);
    }
};
