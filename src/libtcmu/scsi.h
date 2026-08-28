/*
 * Copyright (c) 2014 Red Hat, Inc.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

/*
 * API used by tcmu-runner and example daemons
 */

#pragma once

#include <stdbool.h>
#include <unistd.h>

/* Temporarily limit this to 32M */
#define VPD_MAX_UNMAP_LBA_COUNT            (32 * 1024 * 1024)
#define VPD_MAX_UNMAP_BLOCK_DESC_COUNT     0x04
/* Temporarily limit this is 0x1 */
#define MAX_CAW_LENGTH                     0x01

#define VPD_MAX_WRITE_SAME_LENGTH 0xFFFFFFFF

/* Basic implementations of mandatory SCSI commands */
bool char_to_hex(unsigned char *val, char c);
struct tcmur_handler *tcmu_get_runner_handler(struct tcmu_device *dev);
int tcmu_emulate_inquiry(struct tcmu_device *dev, struct tgt_port *port, uint8_t *cdb, struct iovec *iovec, size_t iov_cnt);
int tcmu_emulate_start_stop(struct tcmu_device *dev, uint8_t *cdb);
int tcmu_emulate_test_unit_ready(uint8_t *cdb, struct iovec *iovec, size_t iov_cnt);
int tcmu_emulate_read_capacity_10(uint64_t num_lbas, uint32_t block_size, uint8_t *cdb,
				  struct iovec *iovec, size_t iov_cnt);
int tcmu_emulate_read_capacity_16(uint64_t num_lbas, uint32_t block_size, uint8_t *cdb,
				  struct iovec *iovec, size_t iov_cnt);
int tcmu_emulate_mode_sense(struct tcmu_device *dev, uint8_t *cdb,
			    struct iovec *iovec, size_t iov_cnt);
int tcmu_emulate_mode_select(struct tcmu_device *dev, uint8_t *cdb,
			     struct iovec *iovec, size_t iov_cnt);
