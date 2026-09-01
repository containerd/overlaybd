/*
 * Copyright (c) 2014 Red Hat, Inc.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

/*
 * APIs for both libtcmu users and tcmu-runner plugins to use.
 */

#pragma once

#include <stdbool.h>
#include <pthread.h>

struct tcmu_device;
struct tgt_port;
struct tcmulib_cmd;

/*
 * TCMU return status codes
 */
enum {
	TCMU_STS_ASYNC_HANDLED = -2,
	TCMU_STS_NOT_HANDLED = -1,
	TCMU_STS_OK = 0,
	TCMU_STS_NO_RESOURCE,
	/* handler has setup sense. */
	TCMU_STS_PASSTHROUGH_ERR,
	TCMU_STS_BUSY,
	TCMU_STS_WR_ERR,
	TCMU_STS_RD_ERR,
	TCMU_STS_MISCOMPARE,
	TCMU_STS_INVALID_CMD,
	TCMU_STS_INVALID_CDB,
	TCMU_STS_INVALID_PARAM_LIST,
	TCMU_STS_INVALID_PARAM_LIST_LEN,
	TCMU_STS_TIMEOUT,
	TCMU_STS_FENCED,
	TCMU_STS_HW_ERR,
	TCMU_STS_RANGE,
	TCMU_STS_FRMT_IN_PROGRESS,
	TCMU_STS_CAPACITY_CHANGED,
	TCMU_STS_NOTSUPP_SAVE_PARAMS,
	TCMU_STS_WR_ERR_INCOMPAT_FRMT,
	TCMU_STS_TRANSITION,
	TCMU_STS_IMPL_TRANSITION_ERR,
	TCMU_STS_EXPL_TRANSITION_ERR,
	TCMU_STS_NO_LOCK_HOLDERS,
	/* xcopy specific errors */
	TCMU_STS_NOTSUPP_SEG_DESC_TYPE,
	TCMU_STS_NOTSUPP_TGT_DESC_TYPE,
	TCMU_STS_CP_TGT_DEV_NOTCONN,
	TCMU_STS_INVALID_CP_TGT_DEV_TYPE,
	TCMU_STS_TOO_MANY_SEG_DESC,
	TCMU_STS_TOO_MANY_TGT_DESC,
};

#define TCMU_THREAD_NAME_LEN 16

#define SENSE_BUFFERSIZE 96

#define CFGFS_ROOT "/sys/kernel/config/target"
#define CFGFS_CORE CFGFS_ROOT"/core"

#define CFGFS_TARGET_MOD "/sys/module/target_core_user"
#define CFGFS_MOD_PARAM CFGFS_TARGET_MOD"/parameters"



#define round_up(a, b) ({		\
	__typeof__ (a) _a = (a);	\
	__typeof__ (b) _b = (b);	\
	((_a + (_b - 1)) / _b) * _b; })

#define round_down(a, b) ({		\
	__typeof__ (a) _a = (a);	\
	__typeof__ (b) _b = (b);	\
	(_a - (_a % _b)); })

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

struct tcmulib_cmd {
	uint16_t cmd_id;
	uint8_t *cdb;
	struct iovec *iovec;
	size_t iov_cnt;
	uint8_t sense_buf[SENSE_BUFFERSIZE];
	void *hm_private;
	struct tcmulib_cmd *work_next;

	/* Intrusive link and result used by the multi-VCPU completion queue. */
	struct tcmulib_cmd *completion_next;
	int completion_result;
};

/* Set/Get methods for the opaque tcmu_device */
void *tcmu_dev_get_private(struct tcmu_device *dev);
void tcmu_dev_set_private(struct tcmu_device *dev, void *priv);
const char *tcmu_dev_get_uio_name(struct tcmu_device *dev);
void tcmu_set_thread_name(const char *prefix, struct tcmu_device *dev);
int tcmu_dev_get_fd(struct tcmu_device *dev);
char *tcmu_dev_get_memory_info(struct tcmu_device *dev, void **base,
			       size_t *len, off_t *offset);
char *tcmu_dev_get_cfgstring(struct tcmu_device *dev);
void tcmu_dev_set_num_lbas(struct tcmu_device *dev, uint64_t num_lbas);
uint64_t tcmu_dev_get_num_lbas(struct tcmu_device *dev);
void tcmu_dev_set_block_size(struct tcmu_device *dev, uint32_t block_size);
uint32_t tcmu_dev_get_block_size(struct tcmu_device *dev);
uint64_t tcmu_lba_to_byte(struct tcmu_device *dev, uint64_t lba);
uint64_t tcmu_byte_to_lba(struct tcmu_device *dev, uint64_t byte);
void tcmu_dev_set_max_xfer_len(struct tcmu_device *dev, uint32_t len);
uint32_t tcmu_dev_get_max_xfer_len(struct tcmu_device *dev);
void tcmu_dev_set_opt_xcopy_rw_len(struct tcmu_device *dev, uint32_t len);
uint32_t tcmu_dev_get_opt_xcopy_rw_len(struct tcmu_device *dev);
void tcmu_dev_set_max_unmap_len(struct tcmu_device *dev, uint32_t len);
uint32_t tcmu_dev_get_max_unmap_len(struct tcmu_device *dev);
void tcmu_dev_set_opt_unmap_gran(struct tcmu_device *dev, uint32_t len,
				 bool split);
uint32_t tcmu_dev_get_opt_unmap_gran(struct tcmu_device *dev);
void tcmu_dev_set_unmap_gran_align(struct tcmu_device *dev, uint32_t len);
uint32_t tcmu_dev_get_unmap_gran_align(struct tcmu_device *dev);
void tcmu_dev_set_write_cache_enabled(struct tcmu_device *dev, bool enabled);
bool tcmu_dev_get_write_cache_enabled(struct tcmu_device *dev);
void tcmu_dev_set_solid_state_media(struct tcmu_device *dev, bool solid_state);
bool tcmu_dev_get_solid_state_media(struct tcmu_device *dev);
void tcmu_dev_set_unmap_enabled(struct tcmu_device *dev, bool enabled);
bool tcmu_dev_get_unmap_enabled(struct tcmu_device *dev);
void tcmu_dev_set_write_protect_enabled(struct tcmu_device *dev, bool enabled);
bool tcmu_dev_get_write_protect_enabled(struct tcmu_device *dev);
struct tcmulib_handler *tcmu_dev_get_handler(struct tcmu_device *dev);
void tcmu_dev_flush_ring(struct tcmu_device *dev);
bool tcmu_dev_oooc_supported(struct tcmu_device* dev);

/* Set/Get methods for interacting with configfs */
char *tcmu_cfgfs_get_str(const char *path);
int tcmu_cfgfs_set_str(const char *path, const char *val, int val_len);
int tcmu_cfgfs_get_int(const char *path);
int tcmu_cfgfs_set_u32(const char *path, uint32_t val);
int tcmu_cfgfs_dev_get_attr_int(struct tcmu_device *dev, const char *name);
int tcmu_cfgfs_dev_exec_action(struct tcmu_device *dev, const char *name,
			       uint32_t val);
int tcmu_cfgfs_dev_set_ctrl_u64(struct tcmu_device *dev, const char *key,
			        uint64_t val);
uint64_t tcmu_cfgfs_dev_get_info_u64(struct tcmu_device *dev, const char *name,
                                     int *fn_ret);
char *tcmu_cfgfs_dev_get_wwn(struct tcmu_device *dev);
int tcmu_cfgfs_mod_param_set_u32(const char *name, uint32_t val);

/* Helper routines for processing commands */

/* SCSI CDB processing */
int tcmu_cdb_get_length(uint8_t *cdb);
uint64_t tcmu_cdb_get_lba(uint8_t *cdb);
uint32_t tcmu_cdb_get_xfer_length(uint8_t *cdb);
void tcmu_cdb_print_info(struct tcmu_device *dev, const struct tcmulib_cmd *cmd,
			 const char *info);
uint64_t tcmu_cdb_to_byte(struct tcmu_device *dev, uint8_t *cdb);

/* iovec processing */
off_t tcmu_iovec_compare(void *mem, struct iovec *iovec, size_t size);
size_t tcmu_iovec_seek(struct iovec *iovec, size_t count);
void tcmu_iovec_zero(struct iovec *iovec, size_t iov_cnt);
bool tcmu_iovec_zeroed(struct iovec *iovec, size_t iov_cnt);
size_t tcmu_iovec_length(struct iovec *iovec, size_t iov_cnt);

/* memory mangement */
size_t tcmu_memcpy_into_iovec(struct iovec *iovec, size_t iov_cnt, void *src,
			      size_t len);
size_t tcmu_memcpy_from_iovec(void *dest, size_t len, struct iovec *iovec,
			      size_t iov_cnt);

/* tcmulib_cmd processing */
void tcmu_cmd_seek(struct tcmulib_cmd *cmd, size_t count);

/* SCSI Sense */
int tcmu_sense_set_data(uint8_t *sense_buf, uint8_t key, uint16_t asc_ascq);
void tcmu_sense_set_info(uint8_t *sense_buf, uint32_t info);
void tcmu_sense_set_key_specific_info(uint8_t *sense_buf, uint16_t info);
void __tcmu_sense_set_data(uint8_t *sense_buf, uint8_t key, uint16_t asc_ascq);

/*
 * Misc
 */
void tcmu_thread_cancel(pthread_t thread);

extern __thread int __tcmu_is_ework_thread;
