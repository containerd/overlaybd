/*
 * Copyright (c) 2014 Red Hat, Inc.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

/*
 * This header defines structures private to libtcmu, and should not
 * be used by anyone else.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <limits.h>

#include <atomic>
#include <vector>
#include <map>
#include <string>

#include <photon/thread/thread.h>

#define KERN_IFACE_VER 2

// The full (private) declaration
struct tcmulib_context {
	std::vector<struct tcmulib_handler> handlers;
	std::map<std::string, struct tcmu_device*> devices;
	struct nl_sock *nl_sock = nullptr;
};

struct tcmu_device {
	int fd;
	photon::mutex completion_lock; /* protects response-ring completion */
	std::atomic<uint32_t> aio_pending_wakeups{0};

	struct tcmu_mailbox *map;
	size_t map_len;

	uint32_t cmd_tail;

	uint64_t num_lbas;
	uint32_t block_size;
	uint32_t block_size_shift;
	uint32_t max_xfer_len;
	uint32_t opt_xcopy_rw_len;
	bool split_unmaps;
	uint32_t max_unmap_len;
	uint32_t opt_unmap_gran;
	uint32_t unmap_gran_align;
	unsigned int write_cache_enabled:1;
	unsigned int solid_state_media:1;
	unsigned int unmap_enabled:1;
	unsigned int write_protect_enabled:1;

	char dev_name[16]; /* e.g. "uio14" */
	char tcm_hba_name[16]; /* e.g. "user_8" */
	char tcm_dev_name[128]; /* e.g. "backup2" */
	char cfgstring[PATH_MAX];

	struct tcmulib_handler *handler;
	struct tcmulib_context *ctx;

	void *hm_private; /* private ptr for handler module */
};
