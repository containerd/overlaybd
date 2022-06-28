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
#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/common/uuid.h>
#include <photon/photon.h>
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
    uint64_t vsize;
    string parent_uuid;
    bool sparse = false;
    std::string data_file_path, index_file_path;

    CLI::App app{"this is overlaybd-create"};
    app.add_option("-u", parent_uuid, "parent uuid");
    app.add_flag("-s", sparse, "create sparse RW layer");
    app.add_option("data_file", data_file_path, "data file path")->type_name("FILEPATH")->required();
    app.add_option("index_file", index_file_path, "index file path")->type_name("FILEPATH")->required();
    app.add_option("vsize", vsize, "virtual size(GB)")->type_name("INT")->check(CLI::PositiveNumber)->required();
    CLI11_PARSE(app, argc, argv);


    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);

    vsize *= 1024 * 1024 * 1024;
    const auto flag = O_RDWR | O_EXCL | O_CREAT;
    const auto mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    IFile* fdata = open_file(data_file_path.c_str(), flag, mode);
    IFile* findex = open_file(index_file_path.c_str(), flag, mode);

    LSMT::LayerInfo args(fdata, findex);
    args.parent_uuid.parse(parent_uuid.c_str(), parent_uuid.size());
    args.virtual_size = vsize;
    args.sparse_rw = sparse;

    auto file = LSMT::create_file_rw(args, false);
    if (!file) {
        fprintf(stderr, "failed to create lsmt file object, possibly I/O error!\n");
        exit(-1);
    }
    delete file;
    delete fdata;
    delete findex;
    printf("lsmt_create has created files SUCCESSFULLY\n");
    return 0;
}
