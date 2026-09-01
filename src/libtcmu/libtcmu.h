/*
 * Copyright (c) 2014 Red Hat, Inc.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

/*
 * This header defines the libtcmu API.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>
#include <vector>
#include "libtcmu_common.h"

enum tcmulib_cfg_type {
	TCMULIB_CFG_DEV_CFGSTR,
	TCMULIB_CFG_DEV_SIZE,
	TCMULIB_CFG_WRITE_CACHE,
};

struct tcmulib_cfg_info {
	enum tcmulib_cfg_type type;

	union {
		uint64_t dev_size;
		char *dev_cfgstring;
		bool write_cache;
	} data;
};

struct tcmulib_handler {
	const char *name;	/* Human-friendly name */
	const char *subtype;	/* Name for cfgstring matching */
	const char *cfg_desc;	/* Description of this backstore's config string */

	struct tcmulib_context *ctx; /* The context this handler is added to,
					used internally by libtcmu. */

	/*
	 * As much as possible, check that the cfgstring will result
	 * in a working device when given to us as dev->cfgstring in
	 * the ->open() call.
	 *
	 * This function is optional but gives configuration tools a
	 * chance to warn users in advance if the device they're
	 * trying to create is invalid.
	 *
	 * Returns true if string is valid. Only if false, set *reason
	 * to a string that says why. The string will be free()ed.
	 * Suggest using asprintf().
	 */
	bool (*check_config)(const char *cfgstring, char **reason);

	int (*reconfig)(struct tcmu_device *dev, struct tcmulib_cfg_info *cfg);

	bool (*update_logdir)(void);

	/* Per-device added/removed callbacks */
	int (*added)(struct tcmu_device *dev);
	void (*removed)(struct tcmu_device *dev);

	void *hm_private; /* private ptr for handler module */
	void *connection; /* private, dbus connection for this subtype */
};

/*
 * APIs for libtcmu only
 *
 * Use these functions to handle TCMU devices and events within an
 * existing program's event loop.
 */

/* Opaque (private) type */
struct tcmulib_context;

/* Claim subtypes you wish to handle. Returns libtcmu's master fd or -error.*/
struct tcmulib_context *tcmulib_initialize(std::vector<struct tcmulib_handler> &handlers);

/* Register to TCMU DBus service, for the claimed subtypes to be configurable
 * in targetcli. */
void tcmulib_register(struct tcmulib_context *ctx);

/* Gets the master file descriptor used by tcmulib. */
int tcmulib_get_master_fd(struct tcmulib_context *ctx);

/*
 * Call this when the master fd becomes ready, from your main thread.
 * Handlers' callbacks may be called before it returns.
 */
int tcmulib_master_fd_ready(struct tcmulib_context *ctx);

/*
 * When a device fd becomes ready, call this to get SCSI cmd info in
 * 'cmd' struct. libtcmu will allocate hm_cmd_size bytes for each cmd
 * that can be accessed via cmd->hm_private pointer. The memory at
 * hm_private will be freed in tcmulib_command_complete.
 *
 * Repeat until it returns false.
 */
struct tcmulib_cmd *tcmulib_get_next_command(struct tcmu_device *dev,
					     int hm_cmd_size);

/*
 * Mark the command as complete.
 * Must be called before get_next_command() is called again.
 *
 * result is TCMU_STS value from libtcmu_common.h. If TCMU_STS_PASSTHROUGH_ERR
 * is returned then the caller must setup the tcmulib_cmd->sense_buf.
 */
void tcmulib_command_complete(struct tcmu_device *dev, struct tcmulib_cmd *cmd, int result);

/* Call when start processing commands (before calling tcmulib_get_next_command()) */
void tcmulib_processing_start(struct tcmu_device *dev);

/* Call when complete processing commands (tcmulib_get_next_command() returned NULL) */
void tcmulib_processing_complete(struct tcmu_device *dev);

/* Clean up loose ends when exiting */
void tcmulib_close(struct tcmulib_context *ctx);
