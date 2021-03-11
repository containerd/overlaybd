/*
 * registryfs.h
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
#include <stdint.h>
#include <string>
#include "../../callback.h"
#include "../virtual-file.h"
namespace FileSystem {

// Docker registry always returns Token with expiry time >= 60 seconds.
// https://docs.docker.com/registry/spec/auth/token/#authorization-server-endpoint-descriptions
// static const uint64_t kMinimumTokenExpiry = 60 * 1000UL;  // 60 seconds

struct DockerLayerMeta {
    uint64_t crc64;
    uint64_t contentLength;
    char lastModified[128];
};

// IFile open by registryfs are RegistryFile, it can cast to RegistryFile* so
// that able to call getMeta & getUrl methods
// Since RegistryFile depends on registryfs to get authorized, it can only open
// using registryfs, and not able to create directly using url
class RegistryFile : public VirtualReadOnlyFile {
public:
    virtual int getMeta(DockerLayerMeta *meta, uint64_t timeout = -1) = 0;
    virtual int getUrl(char *buf, size_t size, uint64_t timeout = -1) = 0;
    int close() override {
        return 0;
    }
};

using PasswordCB = Delegate<std::pair<std::string, std::string>, const char *>;

extern "C" {
IFileSystem *new_registryfs_with_password_callback(const char *baseUrl, PasswordCB callback,
                                                   const char *caFile = nullptr,
                                                   uint64_t timeout = -1);

IFileSystem *new_registryfs_with_password(const char *baseUrl, const char *username,
                                          const char *password, const char *caFile = nullptr,
                                          uint64_t timeout = -1);

IFileSystem *new_registryfs_with_token(const char *baseUrl, const char *token,
                                       const char *caFile = nullptr, uint64_t timeout = -1);

IFileSystem *new_registryfs_without_auth(const char *baseUrl, const char *caFile = nullptr,
                                         uint64_t timeout = -1);
}

} // namespace FileSystem
