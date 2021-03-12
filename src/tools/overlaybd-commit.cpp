/*
 * overlaybd-commit.cpp
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
#include "../overlaybd/fs/localfs.h"
#include "../overlaybd/fs/lsmt/file.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;
using namespace LSMT;
using namespace FileSystem;

static void usage() {
    static const char msg[] =
        "overlaybd-commit [-v|-m msg | -p parent_uuid]  <data file> <index file> [output file]\n"
        "options:\n"
        "   -v print log detail.\n"
        "   -m <msg>    add some custom message if needed.\n"
        "   -p <parent-uuid> parent uuid.\n"
        "example:\n"
        "   ./overlaybd-commit -m commitMsg ./file.data ./file.index ./file.lsmt\n";

    puts(msg);
    exit(0);
}

IFile *open(IFileSystem *fs, const char *fn, int flags, mode_t mode = 0) {
    auto file = fs->open(fn, flags, mode);
    if (!file) {
        fprintf(stderr, "failed to open file '%s', %d: %s\n", fn, errno, strerror(errno));
        exit(-1);
    }
    return file;
}

IFileRW *fin;
IFile *fout;
string commit_msg;
string parent_uuid;

static void parse_args(int argc, char **argv) {
    int shift = 1;
    int ch;
    bool log = false;
    while ((ch = getopt(argc, argv, "vm:p:")) != -1) {
        switch (ch) {
            case 'v':
                log = true;
                shift++;
                break;
            case 'm':
                commit_msg = optarg;
                shift += 2;
                break;
            case 'p':
                if (!UUID::String::is_valid(optarg)) {
                    fprintf(stderr, "invalid UUID format.\n");
                    exit(-1);
                }
                parent_uuid = string(optarg);
                shift += 2;
                break;
            default:
                usage();
                exit(-1);
        }
    }
    if (argc - shift < 1 || argc - shift > 3)
        return usage();

    if (!log)
        log_output = log_output_null;
    unique_ptr<IFileSystem> lfs(new_localfs_adaptor());
    auto fdata = open(lfs.get(), argv[shift], O_RDONLY);
    shift++;
    auto findex = open(lfs.get(), argv[shift], O_RDONLY);
    shift++;
    fin = open_file_rw(fdata, findex, true);
    if (!fin) {
        delete fdata;
        delete findex;
        fprintf(stderr, "failed to create lsmt file object, possibly wrong file format!\n");
        exit(-1);
    }

    fout = (argc - shift == 0) ? new_localfile_adaptor(1) : // stdout
               open(lfs.get(), argv[argc - 1], O_RDWR | O_EXCL | O_CREAT,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
}

int main(int argc, char **argv) {
    parse_args(argc, argv);
    CommitArgs args(fout);
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
    delete fout;
    delete fin;
    printf("lsmt_commit has committed files SUCCESSFULLY\n");
    return ret;
}