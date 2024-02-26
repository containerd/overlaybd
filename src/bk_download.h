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
#include <list>
#include <string>
#include <cstdint>
#include <photon/fs/filesystem.h>

class ImageFile;
class ISwitchFile;

namespace BKDL {

static std::string DOWNLOAD_TMP_NAME = ".download";

bool check_downloaded(const std::string &dir);

class BkDownload {
public:
    std::string dir;
    uint32_t try_cnt;

    bool download();
    bool lock_file();
    void unlock_file();

    BkDownload() = delete;
    ~BkDownload() {
        unlock_file();
        delete src_file;
    }
    BkDownload(ISwitchFile *sw_file, photon::fs::IFile *src_file, size_t file_size,
               const std::string &dir, const std::string &digest, const std::string &url,
               int &running, int32_t limit_MB_ps, int32_t try_cnt, uint32_t bs)
        : dir(dir), try_cnt(try_cnt), sw_file(sw_file), src_file(src_file),
          file_size(file_size), digest(digest), url(url), running(running),
          limit_MB_ps(limit_MB_ps), block_size(bs) {
    }

private:
    void switch_to_local_file();
    bool download_blob();
    bool download_done();

    ISwitchFile *sw_file = nullptr;
    photon::fs::IFile *src_file = nullptr;
    size_t file_size;
    std::string digest;
    std::string url;
    int &running;
    int32_t limit_MB_ps;
    uint32_t block_size;
    bool force_download = false;
};

void bk_download_proc(std::list<BKDL::BkDownload *> &, uint64_t, int &);

} // namespace BKDL