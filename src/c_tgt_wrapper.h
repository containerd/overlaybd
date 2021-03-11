/*
 * c_tgt_wrapper.h
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

#ifndef _c_wrapper_h
#define _c_wrapper_h

#include <stdint.h>
#include <unistd.h>

#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

struct scsi_cmd;
struct ImageFile;

typedef void(request_cb_t)(struct scsi_cmd *, uint32_t ret);

EXTERNC struct ImageFile *ex_perform_get_ifile(char *config_path, uint64_t *size, int *ro,
                                               uint32_t *blksize);
EXTERNC int ex_perform_ifile_close(struct ImageFile *);
EXTERNC int ex_perform_ifile_exit(struct ImageFile *);

EXTERNC void ex_async_read(struct ImageFile *, void *buf, size_t count, off_t offset,
                           struct scsi_cmd *cmd, request_cb_t *func);
EXTERNC void ex_async_write(struct ImageFile *, void *buf, size_t count, off_t offset,
                            struct scsi_cmd *cmd, request_cb_t *func);
EXTERNC void ex_async_sync(struct ImageFile *, struct scsi_cmd *cmd, request_cb_t *func);
EXTERNC void ex_async_unmap(struct ImageFile *fd, off_t offset, size_t len, struct scsi_cmd *cmd,
                            request_cb_t *func);

EXTERNC void *init_finish_queue();
EXTERNC void push_finish_queue(void *queue, struct scsi_cmd *cmd);
EXTERNC struct scsi_cmd *pop_finish_queue(void *queue);
EXTERNC void delete_finish_queue(void *queue);

#endif
