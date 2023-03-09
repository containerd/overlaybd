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
#include <photon/common/alog-stdstring.h>
#include <photon/fs/localfs.h>
#include <photon/photon.h>
#include "../overlaybd/lsmt/file.h"
#include "../overlaybd/zfile/zfile.h"
#include "../overlaybd/tar_file.h"
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


IFile *open_file(IFileSystem *fs, const char *fn, int flags, mode_t mode = 0) {
    auto file = fs->open(fn, flags, mode);
    if (!file) {
        fprintf(stderr, "failed to open file '%s', %d: %s\n", fn, errno, strerror(errno));
        exit(-1);
    }
    return file;
}

int main(int argc, char **argv) {
    string commit_msg;
    string parent_uuid;
    std::string algorithm;
    int block_size = -1;
    std::string data_file_path, index_file_path, commit_file_path, remote_mapping_file;
    bool compress_zfile = false;
    bool build_fastoci = false;
    bool tar = false, rm_old = false;

    CLI::App app{"this is overlaybd-commit"};
    app.add_option("-m", commit_msg, "add some custom message if needed");
    app.add_option("-p", parent_uuid, "parent uuid");
    app.add_flag("-z", compress_zfile, "compress to zfile");
    app.add_flag("-t", tar, "wrapper with tar");
    app.add_flag("-f", rm_old, "force compress. unlink exist");
    app.add_option("--algorithm", algorithm, "compress algorithm, [lz4|zstd](default lz4)");
    app.add_option(
           "--bs", block_size,
           "The size of a data block in KB. Must be a power of two between 4K~64K [4/8/16/32/64](default 4)");
    app.add_flag("--fastoci", build_fastoci, "commit using fastoci format");
    app.add_option("data_file", data_file_path, "data file path")->type_name("FILEPATH")->check(CLI::ExistingFile)->required();
    app.add_option("index_file", index_file_path, "index file path")->type_name("FILEPATH")->check(CLI::ExistingFile)->required();
    app.add_option("commit_file", commit_file_path, "commit file path")->type_name("FILEPATH")->required();
    CLI11_PARSE(app, argc, argv);

    set_log_output_level(1);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);

    IFileSystem *lfs = new_localfs_adaptor();
    IFile* fdata = open_file(lfs, data_file_path.c_str(), O_RDONLY, 0);
    IFile* findex = open_file(lfs, index_file_path.c_str(), O_RDONLY, 0);
    IFileRW* fin = nullptr;
    if (build_fastoci) {
        LOG_INFO("commit LSMTWarpFile with args: {index_file: `, fsmeta: `",
            index_file_path, data_file_path);
        fin = open_warpfile_rw(findex, fdata, nullptr, true);
    } else {
        fin = open_file_rw(fdata, findex, true);
    }
    if (rm_old) {
        lfs->unlink(commit_file_path.c_str());
    }
    IFile *fout = nullptr;
    IFile *out = nullptr;
    IFile *zfile_builder = nullptr;
    ZFile::CompressOptions opt;
    opt.verify = 1;
    if (compress_zfile) {
        if (algorithm == "") {
            algorithm = "lz4";
        }
        if (algorithm == "lz4") {
            opt.type = ZFile::CompressOptions::LZ4;
        } else if (algorithm == "zstd") {
            opt.type = ZFile::CompressOptions::ZSTD;
        } else {
            fprintf(stderr, "invalid '--algorithm' parameters.\n");
            exit(-1);
        }
        if (block_size == -1) {
            block_size = 4;
        }
        opt.block_size = block_size * 1024;
        if ((opt.block_size & (opt.block_size - 1)) != 0 || (block_size > 64 || block_size < 4)) {
            fprintf(stderr, "invalid '--bs' parameters.\n");
            exit(-1);
        }
        IFileSystem *fs = lfs;
        if (tar) {
            fs = new_tar_fs_adaptor(fs);
        }
        fout = open_file(fs, commit_file_path.c_str(), O_RDWR | O_EXCL | O_CREAT,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        ZFile::CompressArgs zfile_args(opt);
        zfile_builder = ZFile::new_zfile_builder(fout, &zfile_args, false);
        out = zfile_builder;
    } else {
        if (algorithm != "" || block_size != 0) {
            fprintf(stderr, "WARNING option '--bs' and '--algorithm' will be ignored without '-z'\n");
        }
        fout = open_file(lfs, commit_file_path.c_str(),  O_RDWR | O_EXCL | O_CREAT,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        out = fout;
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
