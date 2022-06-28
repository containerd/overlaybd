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

#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/photon.h>
#include "../overlaybd/lsmt/file.h"
#include "../overlaybd/zfile/zfile.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <memory>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "CLI11.hpp"

using namespace std;
using namespace LSMT;
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
    string commit_msg;
    string parent_uuid;
    std::string data_file_path, index_file_path, commit_file_path;
    bool compress_zfile = false;

    CLI::App app{"this is overlaybd-commit"};
    app.add_option("-m", commit_msg, "add some custom message if needed");
    app.add_option("-p", parent_uuid, "parent uuid");
    app.add_flag("-z", compress_zfile, "compress to zfile");
    app.add_option("data_file", data_file_path, "data file path")->type_name("FILEPATH")->check(CLI::ExistingFile)->required();
    app.add_option("index_file", index_file_path, "index file path")->type_name("FILEPATH")->check(CLI::ExistingFile)->required();
    app.add_option("commit_file", commit_file_path, "commit file path")->type_name("FILEPATH")->required();
    CLI11_PARSE(app, argc, argv);

    set_log_output_level(1);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);


    IFile* fdata = open_file(data_file_path.c_str(), O_RDONLY, 0);
    IFile* findex = open_file(index_file_path.c_str(), O_RDONLY, 0);
    IFileRW* fin = open_file_rw(fdata, findex, true);
    IFile* fout = open_file(commit_file_path.c_str(),  O_RDWR | O_EXCL | O_CREAT,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    auto out = fout;
    IFile *zfile_builder = nullptr;
    ZFile::CompressOptions opt;
    opt.verify = 1;
    ZFile::CompressArgs zfile_args(opt);
    if (compress_zfile) {
        zfile_builder = ZFile::new_zfile_builder(out, &zfile_args, false);
        out = zfile_builder;
    }

    CommitArgs args(out);
    if (parent_uuid.empty() == false) {
        memcpy(args.parent_uuid.data, parent_uuid.c_str(), parent_uuid.length());
    }
    if (commit_msg != "") {
        args.user_tag = const_cast<char *>(commit_msg.c_str());
    }
    auto ret = fin->commit(args);
    if (ret < 0) {
        fprintf(stderr, "failed to perform commit(), %d: %s\n", errno, strerror(errno));
    }
    out->close();
    delete zfile_builder;
    delete fout;
    delete fin;
    printf("lsmt_commit has committed files SUCCESSFULLY\n");
    return ret;
}
