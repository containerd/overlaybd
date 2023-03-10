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

#include "../overlaybd/zfile/zfile.h"
#include "../overlaybd/tar_file.h"
#include <photon/common/uuid.h>
#include <photon/common/utility.h>
#include <photon/fs/localfs.h>
#include <photon/common/alog.h>
#include <cstdio>
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <memory>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <photon/photon.h>
#include "CLI11.hpp"

using namespace std;
using namespace photon::fs;
using namespace ZFile;

IFileSystem *lfs = nullptr;

int main(int argc, char **argv) {

    bool rm_old = false;
    bool tar = false;
    bool extract = false;
    std::string fn_src, fn_dst;
    std::string algorithm;
    int block_size;

    CLI::App app{"this is a zfile tool to create/extract zfile"};
    app.add_flag("-t", tar, "wrapper with tar")->default_val(false);
    app.add_flag("-x", extract, "extract zfile")->default_val(false);
    app.add_flag("-f", rm_old, "force compress. unlink exist")->default_val(false);
    app.add_option("--algorithm", algorithm, "compress algorithm, [lz4|zstd]")->default_str("lz4");
    app.add_option(
           "--bs", block_size,
           "The size of a data block in KB. Must be a power of two between 4K~64K [4/8/16/32/64])")
        ->default_val(4);
    app.add_option("source_file", fn_src, "source file path")
        ->type_name("FILEPATH")
        ->check(CLI::ExistingFile)
        ->required();
    app.add_option("target_file", fn_dst, "target file path")->type_name("FILEPATH")->required();
    CLI11_PARSE(app, argc, argv);

    set_log_output_level(1);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);

    CompressOptions opt;
    opt.verify = 1;
    if (algorithm == "lz4") {
        opt.type = CompressOptions::LZ4;
    } else if (algorithm == "zstd") {
        opt.type = CompressOptions::ZSTD;
    }
    opt.block_size = block_size * 1024;
    if ((opt.block_size & (opt.block_size - 1)) != 0 || (block_size > 64 || block_size < 4)) {
        fprintf(stderr, "invalid '--bs' parameters.\nj");
        exit(-1);
    }
    lfs = new_localfs_adaptor();
    if (rm_old) {
        lfs->unlink(fn_dst.c_str());
    }
    IFileSystem *fs = lfs;
    if (tar) {
        fs = new_tar_fs_adaptor(lfs);
    }
    int ret = 0;
    CompressArgs args(opt);
    if (!extract) {
        printf("compress file %s as %s\n", fn_src.c_str(), fn_dst.c_str());
        IFile *infile = lfs->open(fn_src.c_str(), O_RDONLY);
        if (infile == nullptr) {
            fprintf(stderr, "failed to open file %s\n", fn_src.c_str());
            exit(-1);
        }
        DEFER(delete infile);

        IFile *outfile = fs->open(fn_dst.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
        if (outfile == nullptr) {
            fprintf(stderr, "failed to open file %s\n", fn_dst.c_str());
            exit(-1);
        }
        DEFER(delete outfile);

        ret = zfile_compress(infile, outfile, &args);
        if (ret != 0) {
            fprintf(stderr, "compress failed, errno:%d\n", errno);
            exit(-1);
        }
        printf("compress file done.\n");
        return ret;
    } else {
        printf("decompress file %s as %s\n", fn_src.c_str(), fn_dst.c_str());

        IFile *infile = fs->open(fn_src.c_str(), O_RDONLY);
        if (infile == nullptr) {
            fprintf(stderr, "failed to open file %s\n", fn_dst.c_str());
            exit(-1);
        }
        DEFER(delete infile);
        IFile *outfile = lfs->open(fn_dst.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRWXU);
        if (outfile == nullptr) {
            fprintf(stderr, "failed to open file %s\n", fn_dst.c_str());
            exit(-1);
        }
        DEFER(delete outfile);

        ret = zfile_decompress(infile, outfile);
        if (ret != 0) {
            fprintf(stderr, "decompress failed, errno:%d\n", errno);
            exit(-1);
        }
        printf("decompress file done.\n");
        return ret;
    }
}
