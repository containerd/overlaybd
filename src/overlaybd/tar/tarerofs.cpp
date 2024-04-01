#include "tarerofs.h"
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <cstdio>
#include <utime.h>
#include <set>
#include <string>
#include <photon/fs/localfs.h>
#include <photon/fs/path.h>
#include <photon/common/string_view.h>
#include <photon/fs/filesystem.h>
#include <photon/common/alog.h>
#include <photon/fs/fiemap.h>
#include "../lsmt/file.h"
#include "../lsmt/index.h"

#define TAREROFS_BLOCK_SIZE 4096
#define __stringify_1(x...)	#x
#define __stringify(x...)	__stringify_1(x)

#define LSMT_ALIGNMENT 512

int TarErofs::extract_all() {
    ssize_t read;
    struct stat st;
    char buf[128*1024];
    char base_path[64] = "/tmp/tarerofs_base_XXXXXX";
    char command_line[256] = "mkfs.erofs --tar=0,upper.map,1073741824 -b" __stringify(TAREROFS_BLOCK_SIZE) " --aufs";
    const char command_line2[] = " upper.erofs";

    FILE *fp;
    photon::fs::IFile *rawfs = nullptr;
    int status;
    uint64_t blkaddr, toff, metasize;
    uint32_t nblocks;

    if (!meta_only) {
         LOG_ERROR("currently EROFS supports fastoci mode only", strerror(errno));
         return -1;
    }

    if (!first_layer) {
        int fd = mkstemp(base_path);
        if (fd < 0) {
            LOG_ERROR("cannot generate a temporary file to dump overlaybd disk");
            return -1;
        }
        std::strcat(command_line, " --base ");
        std::strcat(command_line, base_path);

        // lsmt.pread should align to 512
        if (fs_base_file->pread(&buf, LSMT_ALIGNMENT, 0) != LSMT_ALIGNMENT) {
            LOG_ERROR("failed to read EROFS metadata size");
            return -1;
        }
        metasize = *(uint64_t *)buf;

        while (metasize) {
            int count = std::min(sizeof(buf), metasize);
            read = fs_base_file->read(buf, count);
            if (read != count ||
                write(fd, buf, read) != read) {
                read = -1;
                break;
            }
            metasize -= read;
        }
        close(fd);
        if (read < 0) {
            return -1;
        }
    }
    std::strcat(command_line, command_line2);
    fp = popen(command_line, "w");
    if (fp == NULL) {
         LOG_ERROR("failed to execute mkfs.erofs", strerror(errno));
         return -1;
    }

    while ((read = file->read(buf, sizeof(buf))) > 0) {
        if (fwrite(buf, read, 1, fp) != 1) {
            read = -1;
            break;
        }
    }
    status = pclose(fp);

    if (!first_layer)
        unlink(base_path);

    if (read < 0 || status) {
        return -1;
    }

    rawfs = photon::fs::open_localfile_adaptor("upper.erofs", O_RDONLY, 0644);
    DEFER({ delete rawfs; });

    /* write to LSMT */
    metasize = rawfs->lseek(0, SEEK_END);
    rawfs->lseek(0, 0);
    fout->lseek(0, 0);
    while ((read = rawfs->read(buf, sizeof(buf))) > 0) {
        if (metasize) {                // since pwrite < 512 is unsupported.
            *(uint64_t *)buf = metasize;
            metasize = 0;
        }

        if (fout->write(buf, read) != read) {
            read = -1;
            break;
        }
    }

    /* write mapfile */
    fp = fopen("upper.map", "r");
    if (fp == NULL) {
       LOG_ERROR("unable to get upper.map, ignored");
       return -1;
    }
    while (fscanf(fp, "%" PRIx64" %x %" PRIx64 "\n", &blkaddr, &nblocks, &toff) >= 3) {
        LSMT::RemoteMapping lba;
        lba.offset = blkaddr * TAREROFS_BLOCK_SIZE;
        lba.count = nblocks * TAREROFS_BLOCK_SIZE;
        lba.roffset = toff;
        int nwrite = fout->ioctl(LSMT::IFileRW::RemoteData, lba);
        if ((unsigned) nwrite != lba.count) {
            LOG_ERRNO_RETURN(0, -1, "failed to write lba");
        }
    }
    fclose(fp);
    return 0;
}
