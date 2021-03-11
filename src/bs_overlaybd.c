/*
 * tgt backing store of overlaybd
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

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/epoll.h>

#include "list.h"
#include "tgtd.h"
#include "scsi.h"
#include "spc.h"
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <malloc.h>

#include "c_tgt_wrapper.h"

#define MAX_CONFIG_PATH_LENGTH (4096)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

struct bs_overlaybd_info {
    struct scsi_lu *lu;
    struct ImageFile *ifile;
    size_t size;
    int evt_fd;
    void *fq;
};

static inline struct bs_overlaybd_info *BS_overlaybd_I(struct scsi_lu *lu) {
    return (struct bs_overlaybd_info *)((char *)lu + sizeof(*lu));
}

static void bs_get_completions(int fd, int events, void *data) {
    struct bs_overlaybd_info *info = data;
    int ret;
    /* read from eventfd returns 8-byte int, fails with the error EINVAL
       if the size of the supplied buffer is less than 8 bytes */
    uint64_t evts_complete;
    unsigned int ncomplete;

retry_read:
    ret = read(info->evt_fd, &evts_complete, sizeof(evts_complete));
    if (unlikely(ret < 0)) {
        eprintf("failed to read overlaybd completions, %m\n");
        if (errno == EAGAIN || errno == EINTR)
            goto retry_read;

        return;
    }
    ncomplete = (unsigned int)evts_complete;

    while (ncomplete) {
        struct scsi_cmd *cmd = pop_finish_queue(info->fq);
        target_cmd_io_done(cmd, cmd->result);
        ncomplete--;
    }
}

static int bs_overlaybd_open(struct scsi_lu *lu, char *path, int *fd, uint64_t *size) {
    struct bs_overlaybd_info *info = BS_overlaybd_I(lu);
    uint32_t blksize = 0;
    int ro = 0;

    dprintf("enter bs_overlaybd_open path:%s\n", path);
    struct timeval start_tv, end_tv;
    gettimeofday(&start_tv, NULL);

    int afd = eventfd(0, O_NONBLOCK);
    if (afd < 0) {
        eprintf("failed to create eventfd for %s\n", path);
        goto error_out;
    }
    dprintf("eventfd:%d for %s\n", afd, path);

    int ret = tgt_event_add(afd, EPOLLIN, bs_get_completions, info);
    if (ret)
        goto close_eventfd;
    info->evt_fd = afd;
    info->fq = init_finish_queue();

    info->ifile = ex_perform_get_ifile(path, &(info->size), &ro, &blksize);
    if (info->ifile == NULL) {
        eprintf("failed to call ex_perform_get_ifile\n");
        goto remove_tgt_evt;
    }
    if (ro == 1) {
        lu->attrs.readonly = 1;
    }
    lu->attrs.thinprovisioning = 1;

    *size = (uint64_t)info->size;

    gettimeofday(&end_tv, NULL);
    int64_t time_cost =
        1000000UL * (end_tv.tv_sec - start_tv.tv_sec) + end_tv.tv_usec - start_tv.tv_usec;
    eprintf("overlaybd opened, path: %s, size: %lu, ro: %d, time cost: %ld(ms)\n", path, *size, ro,
            time_cost/1000);

    if (!lu->attrs.no_auto_lbppbe) {
        update_lbppbe(lu, blksize);
    }
    return 0;

remove_tgt_evt:
    tgt_event_del(afd);
close_eventfd:
    close(afd);
    info->evt_fd = 0;

error_out:
    if (info->fq != NULL) {
        delete_finish_queue(info->fq);
        info->fq = NULL;
    }

    return -1;
}

static void bs_overlaybd_close(struct scsi_lu *lu) {
    eprintf("enter bs_overlaybd_close\n");

    struct bs_overlaybd_info *info = BS_overlaybd_I(lu);
    if (info->ifile) {
        ex_perform_ifile_close(info->ifile);
    }
}

static tgtadm_err bs_overlaybd_init(struct scsi_lu *lu, char *bsopts) {
    dprintf("enter bs_overlaybd_init\n");
    struct bs_overlaybd_info *info = BS_overlaybd_I(lu);

    memset(info, 0, sizeof(*info));
    info->lu = lu;

    return TGTADM_SUCCESS;
}

static void bs_overlaybd_exit(struct scsi_lu *lu) {
    dprintf("enter bs_overlaybd_exit\n");

    struct bs_overlaybd_info *info = BS_overlaybd_I(lu);
    if (info->ifile) {
        ex_perform_ifile_exit(info->ifile);
        info->ifile = NULL;
    }

    if (info->fq)
        delete_finish_queue(info->fq);

    if (info->evt_fd) {
        tgt_event_del(info->evt_fd);
        close(info->evt_fd);
    }
}

static void async_callback(struct scsi_cmd *cmd, uint32_t ret) {
    int result;
    int length;

    switch (cmd->scb[0]) {
        case SYNCHRONIZE_CACHE:
        case SYNCHRONIZE_CACHE_16:
        case UNMAP:
        case WRITE_SAME:
        case WRITE_SAME_16:
            length = 0;
            break;

        case WRITE_6:
        case WRITE_10:
        case WRITE_12:
        case WRITE_16:
            length = scsi_get_out_length(cmd);
            break;

        default:
            length = scsi_get_in_length(cmd);
            break;
    }

    if (likely(ret == length))
        result = SAM_STAT_GOOD;
    else {
        sense_data_build(cmd, MEDIUM_ERROR, 0);
        result = SAM_STAT_CHECK_CONDITION;
    }
    scsi_set_result(cmd, result);

    dprintf("overlaybd io done %x %x %d %d\n", result, cmd->scb[0], ret, length);

    struct bs_overlaybd_info *info = BS_overlaybd_I(cmd->dev);
    push_finish_queue(info->fq, cmd);
    uint64_t x = 1;
    write(info->evt_fd, &x, sizeof(x));
}

static int bs_overlaybd_cmd_submit(struct scsi_cmd *cmd) {
    unsigned int scsi_op = (unsigned int)cmd->scb[0];
    struct ImageFile *fd = BS_overlaybd_I(cmd->dev)->ifile;

    switch (scsi_op) {
        case WRITE_6:
        case WRITE_10:
        case WRITE_12:
        case WRITE_16:
            ex_async_write(fd, scsi_get_out_buffer(cmd), scsi_get_out_length(cmd), cmd->offset, cmd,
                           async_callback);
            set_cmd_async(cmd);
            break;

        case READ_6:
        case READ_10:
        case READ_12:
        case READ_16:
            ex_async_read(fd, scsi_get_in_buffer(cmd), scsi_get_in_length(cmd), cmd->offset, cmd,
                          async_callback);
            set_cmd_async(cmd);
            break;

        case SYNCHRONIZE_CACHE:
        case SYNCHRONIZE_CACHE_16:
            if (BS_overlaybd_I(cmd->dev)->lu->attrs.readonly) {
                // return if ro
                scsi_set_result(cmd, SAM_STAT_GOOD);
            } else {
                ex_async_sync(fd, cmd, async_callback);
                set_cmd_async(cmd);
            }
            break;

        case WRITE_SAME:
        case WRITE_SAME_16:
            /* WRITE_SAME used to punch hole in file */
            if (cmd->scb[1] & 0x08) {
                ex_async_unmap(fd, cmd->offset, cmd->tl, cmd, async_callback);
                set_cmd_async(cmd);
                break;
            }
            eprintf("skipped write_same cmd:%p op:%x\n", cmd, scsi_op);
            break;

        case UNMAP:
            eprintf("skipped unmap cmd:%p op:%x\n", cmd, scsi_op);
            break;

        default:
            eprintf("skipped cmd:%p op:%x\n", cmd, scsi_op);
    }

    return 0;
}

static struct backingstore_template obd_bst = {
    .bs_name = "overlaybd",
    .bs_datasize = sizeof(struct bs_overlaybd_info),
    .bs_open = bs_overlaybd_open,
    .bs_close = bs_overlaybd_close,
    .bs_init = bs_overlaybd_init,
    .bs_exit = bs_overlaybd_exit,
    .bs_cmd_submit = bs_overlaybd_cmd_submit,
};

void register_bs_module(void) {
    eprintf("register overlaybd backing-store\n");
    register_backingstore_template(&obd_bst);

    eprintf("mallopt, return:%d\n", mallopt(M_TRIM_THRESHOLD, 128 * 1024));
}
