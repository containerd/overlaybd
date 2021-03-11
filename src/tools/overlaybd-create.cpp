/*
 * overlaybd-create.cpp
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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <memory>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../overlaybd/fs/lsmt/file.h"
#include "../overlaybd/fs/localfs.h"
#include "../overlaybd/alog.h"
#include "../overlaybd/uuid.h"

using namespace std;
using namespace FileSystem;

static void usage() {
    static const char msg[] =
        "overlaybd-create [options] <data file> <index file> <virtual size in GB>\n"
        "options:\n"
        "   -u <parent_UUID>\n"
        "example:\n"
        "   ./overlaybd-create ./file.data ./file.index 100\n";

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

uint64_t vsize;
int level = 255;
unique_ptr<IFile> fdata, findex;
string parent_uuid;
static void parse_args(int argc, char **argv) {
    int shift = 1;
    int ch;
    while ((ch = getopt(argc, argv, "u:")) != -1) {
        switch (ch) {
            case 'u':
                parent_uuid = optarg;
                shift += 2;
                break;
            default:
                usage();
                exit(-1);
        }
    }
    if (argc - shift < 2)
        return usage();

    const auto flag = O_RDWR | O_EXCL | O_CREAT;
    const auto mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    unique_ptr<IFileSystem> lfs(new_localfs_adaptor());
    auto fdata = open(lfs.get(), argv[shift++], flag, mode);
    auto findex = open(lfs.get(), argv[shift++], flag, mode);
    ::findex.reset(findex);
    ::fdata.reset(fdata);

    int ret = sscanf(argv[shift++], "%lu", &vsize);
    if (ret != 1) {
        fprintf(stderr, "failed to parse virtual size: '%s'\n", argv[shift - 1]);
        exit(-1);
    }
    vsize *= 1024 * 1024 * 1024;
    if (shift == argc)
        return;
}

int main(int argc, char **argv) {
    log_output = log_output_null;
    parse_args(argc, argv);
    LSMT::LayerInfo args(fdata.get(), findex.get());
    args.parent_uuid.parse(parent_uuid.c_str(), parent_uuid.size());
    args.virtual_size = vsize;

    auto file = LSMT::create_file_rw(args, false);
    if (!file) {
        fprintf(stderr, "failed to create lsmt file object, possibly I/O error!\n");
        exit(-1);
    }
    delete file;
    printf("lsmt_create has created files SUCCESSFULLY\n");
    return 0;
}
