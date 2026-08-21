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
#include "pool_placeholder.h"

#include "../overlaybd/lsmt/file.h"

#include <photon/common/alog.h>
#include <photon/fs/localfs.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

// mirrors overlaybd-create's sparse path: open data/index, create a sparse RW
// LSMT layer of the requested virtual size, close it again
static int create_sparse_layer(const std::string &data_path,
                               const std::string &index_path, uint64_t vsize) {
    const int flag = O_RDWR | O_EXCL | O_CREAT;
    const mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    photon::fs::IFile *fdata = photon::fs::open_localfile_adaptor(data_path.c_str(), flag, mode);
    if (fdata == nullptr) {
        LOG_ERROR("cannot create placeholder data `: `", data_path.c_str(), strerror(errno));
        return -1;
    }
    photon::fs::IFile *findex =
        photon::fs::open_localfile_adaptor(index_path.c_str(), flag, mode);
    if (findex == nullptr) {
        LOG_ERROR("cannot create placeholder index `: `", index_path.c_str(), strerror(errno));
        delete fdata;
        return -1;
    }
    LSMT::LayerInfo args(fdata, findex);
    args.virtual_size = vsize;
    args.sparse_rw = true;
    LSMT::IFileRW *file = LSMT::create_file_rw(args, false);
    if (file == nullptr) {
        LOG_ERROR("create_file_rw failed for placeholder `", data_path.c_str());
        delete fdata;
        delete findex;
        return -1;
    }
    delete file; // closes the layer; fdata/findex are not owned by it
    delete fdata;
    delete findex;
    return 0;
}

static bool file_exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

int ublk_pool_placeholder(const std::string &dir, uint64_t size_gb, int slot,
                          std::string &out_config) {
    std::string base = dir + "/" + std::to_string(size_gb) + "-" + std::to_string(slot);
    std::string data_path = base + ".data";
    std::string index_path = base + ".index";
    std::string config_path = base + ".json";

    // note: a sparse layer's index file is legitimately 0 bytes, so existence
    // (not size) is the reuse criterion
    if (file_exists(data_path) && file_exists(index_path) && file_exists(config_path)) {
        out_config = config_path; // reuse across daemon restarts
        return 0;
    }
    // partial leftovers would fail O_EXCL; start clean
    unlink(data_path.c_str());
    unlink(index_path.c_str());
    unlink(config_path.c_str());

    if (create_sparse_layer(data_path, index_path, size_gb << 30) != 0)
        return -1;

    // minimal writable image config: no lowers, a sparse upper
    std::ofstream out(config_path, std::ios::trunc);
    if (!out) {
        LOG_ERROR("cannot write placeholder config `", config_path.c_str());
        return -1;
    }
    out << "{\"lowers\":[],\"upper\":{\"index\":\"" << index_path << "\",\"data\":\""
        << data_path << "\"}}\n";
    if (!out) {
        LOG_ERROR("failed writing placeholder config `", config_path.c_str());
        return -1;
    }
    out.close();
    LOG_INFO("built pool placeholder image ` (` GB slot `)", config_path.c_str(),
             size_gb, slot);
    out_config = config_path;
    return 0;
}
