/*
 * overlaybd-zfile.cpp
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
#include "../overlaybd/alog.h"
#include "../overlaybd/fs/filesystem.h"
#include "../overlaybd/fs/localfs.h"
#include "../overlaybd/fs/virtual-file.h"
#include "../overlaybd/fs/zfile/zfile.h"
#include "../overlaybd/fs/zfile/tar_zfile.h"
#include "../overlaybd/utility.h"
#include "../overlaybd/uuid.h"
#include <cstdio>
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <memory>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;
using namespace FileSystem;
using namespace ZFile;

int usage() {
    static const char msg[] = "overlaybd-zfile is a tool to create/extract zfile. \n"
                              "Usage: overlaybd-zfile [options] <src_file> <dst_file>\n"
                              "   -d show debug log.\n"
                              "   -f force compress. unlink exist <dst_file>.\n"
                              "   -x extract zfile.\n"
                              "   -t wrapper with tar.\n"
                              "example:\n"
                              "- create\n"
                              "   ./overlaybd-zfile ./layer0.lsmt ./layer0.lsmtz\n"
                              "- extract\n"
                              "   ./overlaybd-zfile -x ./layer0.lsmtz ./layer0.lsmt\n";
    puts(msg);
    return 0;
}

FileSystem::IFileSystem *lfs = nullptr;

int main(int argc, char **argv) {
    log_output_level = 1;
    int ch;
    int op = 0;
    int parse_idx = 1;
    bool rm_old = false;
    bool tar = false;
    CompressOptions opt;
    opt.verify = 1;
    while ((ch = getopt(argc, argv, "tfxd:")) != -1) {
        switch (ch) {
            case 'd':
                printf("set log output level: %d\n", log_output_level);
                log_output_level = 0;
                parse_idx++;
                break;
            case 'x':
                op = 1;
                parse_idx++;
                break;
            case 'f':
                parse_idx++;
                rm_old = true;
                break;
            case 't':
                parse_idx++;
                tar = true;
                break;
            default:
                usage();
                exit(-1);
        }
    }
    lfs = new_localfs_adaptor();
    auto fn_src = argv[parse_idx++];
    auto fn_dst = argv[parse_idx++];
    if (rm_old) {
        lfs->unlink(fn_dst);
    }
    IFileSystem *fs = lfs;
    if (tar)
        fs = new_tar_zfile_fs_adaptor(lfs);

    int ret = 0;
    CompressArgs args(opt);
    if (op == 0) {
        printf("compress file %s as %s\n", fn_src, fn_dst);
        IFile *infile = lfs->open(fn_src, O_RDONLY);
        if (infile == nullptr) {
            LOG_ERROR_RETURN(0, -1, "open source file error.");
        }
        DEFER(delete infile);

        IFile *outfile = fs->open(fn_dst, O_RDWR | O_CREAT | O_EXCL, 0644);
        if (outfile == nullptr) {
            LOG_ERROR_RETURN(0, -1, "open dst file error.");
        }
        DEFER(delete outfile);

        ret = zfile_compress(infile, outfile, &args);
        if (ret != 0) {
            LOG_ERROR_RETURN(0, -1, "compress fail. (err: `, msg: `)", errno, strerror(errno));
        }
        LOG_INFO("compress file done.");
        return ret;
    } else {
        printf("decompress file %s as %s\n", fn_src, fn_dst);

        IFile *infile = fs->open(fn_src, O_RDONLY);
        if (infile == nullptr) {
            LOG_ERROR_RETURN(0, -1, "open source file error.");
        }
        DEFER(delete infile);

        IFile *outfile = lfs->open(fn_dst, O_WRONLY | O_CREAT | O_EXCL, S_IRWXU);
        if (outfile == nullptr) {
            LOG_ERROR_RETURN(0, -1, "open dst file error.");
        }
        DEFER(delete outfile);

        ret = zfile_decompress(infile, outfile);
        if (ret != 0) {
            LOG_ERROR_RETURN(0, -1, "decompress fail. (err: `, msg: `)", errno, strerror(errno));
        }
        LOG_INFO("decompress file done.");
        return ret;
    }
}
