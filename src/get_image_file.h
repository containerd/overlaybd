/*
 * get_image_file.h
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

typedef enum { io_engine_psync, io_engine_libaio, io_engine_posixaio } IOEngineType;

struct GlobalFs {
    FileSystem::IFileSystem *remote_fs = nullptr;
    bool ready = false;
};

struct ImageFile;

ImageFile *get_image_file(const char *config_path);

int load_cred_from_file(const std::string path, const std::string &remote_path,
                        std::string &username, std::string &password);
