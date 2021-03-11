/*
 * bk_download.h
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
#include <list>
#include <string>
#include "../overlaybd/fs/filesystem.h"
#include "switch_file.h"

class ImageFile;

namespace BKDL {

static std::string DOWNLOAD_TMP_NAME = "overlaybd.download";
static std::string COMMIT_FILE_NAME = "overlaybd.commit";

bool check_downloaded(const std::string &dir);

class BkDownload {
public:
    std::string dir;
    uint32_t try_cnt;

    bool download(int &running);
    bool lock_file();
    void unlock_file();

    BkDownload() = delete;
    ~BkDownload() {
        unlock_file();
    }
    BkDownload(FileSystem::ISwitchFile *sw_file, FileSystem::IFile *src_file, const std::string dir,
               int32_t limit_MB_ps, int32_t try_cnt, ImageFile *image_file)
        : sw_file(sw_file), src_file(src_file), dir(dir), limit_MB_ps(limit_MB_ps),
          try_cnt(try_cnt), image_file(image_file) {
    }

private:
    void switch_to_local_file();
    bool download_blob(int &running);
    bool download_done();

    FileSystem::ISwitchFile *sw_file = nullptr;
    FileSystem::IFile *src_file = nullptr;
    int32_t limit_MB_ps;
    ImageFile *image_file;
};

void bk_download_proc(std::list<BKDL::BkDownload *> &, uint64_t, int &);

} // namespace BKDL