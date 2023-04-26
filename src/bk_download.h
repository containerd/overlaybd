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

    bool download(int &running);
    bool lock_file();
    void unlock_file();

    BkDownload() = delete;
    ~BkDownload() {
        unlock_file();
        delete src_file;
    }
    BkDownload(ISwitchFile *sw_file, photon::fs::IFile *src_file, size_t file_size,
               const std::string dir, int32_t limit_MB_ps, int32_t try_cnt, ImageFile *image_file,
               std::string digest)
        : sw_file(sw_file), src_file(src_file), file_size(file_size), dir(dir),
          limit_MB_ps(limit_MB_ps), try_cnt(try_cnt), image_file(image_file), digest(digest) {
    }

private:
    void switch_to_local_file();
    bool download_blob(int &running);
    bool download_done();

    ISwitchFile *sw_file = nullptr;
    photon::fs::IFile *src_file = nullptr;
    int32_t limit_MB_ps;
    ImageFile *image_file;
    std::string digest;
    size_t file_size;
    bool force_download = false;
};

void bk_download_proc(std::list<BKDL::BkDownload *> &, uint64_t, int &);

} // namespace BKDL