/*
 * Copyright (c) 2017 Red Hat, Inc.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>

#include "libtcmu_common.h"
#include "libtcmu_priv.h"
#include <photon/common/alog.h>

#define CFGFS_BUF_SIZE 4096

int tcmu_cfgfs_get_int(const char *path)
{
	int fd;
	char buf[16];
	ssize_t ret;
	unsigned long val;

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		if (errno == ENOENT) {
			LOG_ERROR("Kernel does not support configfs file `", path);
		} else {
			LOG_ERROR("Could not open configfs file `: `", path, strerror(errno));
		}
		return -errno;
	}

	ret = read(fd, buf, sizeof(buf));
	close(fd);
	if (ret == -1) {
		LOG_ERROR("Could not read configfs to read attribute `: `", path, strerror(errno));
		return -errno;
	}

	val = strtoul(buf, NULL, 0);
	if (val > INT_MAX ) {
		LOG_ERROR("could not convert string ` to value", buf);
		return -EINVAL;
	}

	return val;
}

int tcmu_cfgfs_dev_get_attr_int(struct tcmu_device *dev, const char *name)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), CFGFS_CORE"/%s/%s/attrib/%s",
		 dev->tcm_hba_name, dev->tcm_dev_name, name);
	return tcmu_cfgfs_get_int(path);
}

uint64_t tcmu_cfgfs_dev_get_info_u64(struct tcmu_device *dev, const char *name,
				     int *fn_ret)
{
	int fd;
	char path[PATH_MAX];
	char buf[CFGFS_BUF_SIZE];
	ssize_t ret;
	char *rover;
	char *search_pattern;
	uint64_t val;

	*fn_ret = 0;
	snprintf(path, sizeof(path), CFGFS_CORE"/%s/%s/info",
		 dev->tcm_hba_name, dev->tcm_dev_name);

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		if (errno == ENOENT) {
			LOG_ERROR("Kernel does not support device info file `.", path);
		} else {
			LOG_ERROR("Could not open device info file `: `", path, strerror(errno));
		}
		*fn_ret = -errno;
		return 0;
	}

	ret = read(fd, buf, sizeof(buf));
	close(fd);
	if (ret == -1) {
		LOG_ERROR("Could not read configfs to read dev info: `", strerror(errno));
		*fn_ret = -EINVAL;
		return 0;
	} else if (ret == 0) {
		LOG_ERROR("Invalid device info.");
		*fn_ret = -EINVAL;
		return 0;
	}
	buf[ret-1] = '\0'; /* paranoid? Ensure null terminated */

	if (asprintf(&search_pattern, " %s: ", name) < 0) {
		LOG_ERROR("Could not create search string.");
		*fn_ret = -ENOMEM;
		return 0;
	}

	rover = strstr(buf, search_pattern);
	free(search_pattern);
	if (!rover) {
		LOG_ERROR("Could not find \" `: \" in `: `", name, path, strerror(errno));
		*fn_ret = -EINVAL;
		return 0;
	}
	rover += strlen(name) + 3; /* name plus ':' and spaces before/after */

	val = strtoull(rover, NULL, 0);
	if (val == ULLONG_MAX) {
		LOG_ERROR("Could not get `: `", name, strerror(errno));
		*fn_ret = -EINVAL;
		return 0;
	}

	return val;
}

int tcmu_cfgfs_dev_set_ctrl_u64(struct tcmu_device *dev, const char *key,
				uint64_t val)
{
	char path[PATH_MAX];
	char buf[CFGFS_BUF_SIZE];

	snprintf(path, sizeof(path), CFGFS_CORE"/%s/%s/control",
		 dev->tcm_hba_name, dev->tcm_dev_name);
	snprintf(buf, sizeof(buf), "%s=%" PRIu64"", key, val);

	return tcmu_cfgfs_set_str(path, buf, strlen(buf) + 1);
}

int tcmu_cfgfs_mod_param_set_u32(const char *name, uint32_t val)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), CFGFS_MOD_PARAM"/%s", name);
	return tcmu_cfgfs_set_u32(path, val);
}

/*
 * Return a string that contains the device's WWN, or NULL.
 *
 * Callers must free the result with free().
 */
char *tcmu_cfgfs_dev_get_wwn(struct tcmu_device *dev)
{
	int fd;
	char path[PATH_MAX];
	char buf[CFGFS_BUF_SIZE];
	char *ret_buf;
	int ret;

	snprintf(path, sizeof(path),
		 CFGFS_CORE"/%s/%s/wwn/vpd_unit_serial",
		 dev->tcm_hba_name, dev->tcm_dev_name);

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		if (errno == ENOENT) {
			LOG_ERROR("Kernel does not support unit serial file `", path);
		} else {
			LOG_ERROR("Could not open unit serial file `: `", path, strerror(errno));
		}
		return NULL;
	}

	ret = read(fd, buf, sizeof(buf));
	close(fd);
	if (ret == -1) {
		LOG_ERROR("Could not read configfs to read unit serial: `", strerror(errno));
		return NULL;
	} else if (ret == 0) {
		LOG_ERROR("Invalid VPD serial number.");
		return NULL;
	}

	/* Kill the trailing '\n' */
	buf[ret-1] = '\0';

	/* Skip to the good stuff */
	ret = asprintf(&ret_buf, "%s", &buf[28]);
	if (ret == -1) {
		LOG_ERROR("could not convert string to value: `", strerror(errno));
		return NULL;
	}

	return ret_buf;
}

char *tcmu_cfgfs_get_str(const char *path)
{
	int fd, n;
	char buf[CFGFS_BUF_SIZE];
	ssize_t ret;
	char *val;

	memset(buf, 0, sizeof(buf));
	fd = open(path, O_RDONLY);
	if (fd == -1) {
		if (errno == ENOENT) {
			LOG_ERROR("Kernel does not support configfs file `",  path);
		} else {
			LOG_ERROR("Could not open configfs file `: `", path, strerror(errno));
		}
		return NULL;
	}

	ret = read(fd, buf, sizeof(buf));
	close(fd);
	if (ret == -1) {
		LOG_ERROR("Could not read configfs to read attribute `: `", path, strerror(errno));
		return NULL;
	}

	if (ret == 0)
		return NULL;

	/*
	 * Some files like members will terminate each member/line with a null
	 * char. Except for the last one, replace it with '\n' so parsers will
	 * just see an empty member.
	 */
	if (ret != (ssize_t)strlen(buf)) {
		do {
			n = strlen(buf);
			buf[n] = '\n';
		} while (n < ret);
	}

	/*
	 * Some files like members ends with a null char, but other files like
	 * the alua ones end with a newline.
	 */
	if (buf[ret - 1] == '\n')
		buf[ret - 1] = '\0';

	if (buf[ret - 1] != '\0') {
		if (ret >= CFGFS_BUF_SIZE) {
			LOG_ERROR("Invalid cfgfs file `: not enough space for ending null char.",
				 path);
			return NULL;
		}
		/*
		 * In case the file does "return sprintf()" with no ending
		 * newline add the ending null so we will not crash below.
		 */
		buf[ret] = '\0';
	}

	val = strdup(buf);
	if (!val) {
		LOG_ERROR("could not copy buffer ` : `", buf, strerror(errno));
		return NULL;
	}

	return val;
}

int tcmu_cfgfs_set_str(const char *path, const char *val, int val_len)
{
	int fd;
	ssize_t ret;

	fd = open(path, O_WRONLY);
	if (fd == -1) {
		if (errno == ENOENT) {
			LOG_ERROR("Kernel does not support configfs file `", path);
		} else {
			LOG_ERROR("Could not open configfs file `: `", path, strerror(errno));
		}
		return -errno;
	}

	ret = write(fd, val, val_len);
	close(fd);
	if (ret == -1) {
		LOG_ERROR("Could not write configfs to write attribute `: `",
			 path, strerror(errno));
		return -errno;
	}

	return 0;
}

int tcmu_cfgfs_set_u32(const char *path, uint32_t val)
{
	char buf[20];

	sprintf(buf, "%" PRIu32"", val);
	return tcmu_cfgfs_set_str(path, buf, strlen(buf) + 1);
}

int tcmu_cfgfs_dev_exec_action(struct tcmu_device *dev, const char *name,
			       uint32_t val)
{
	char path[PATH_MAX];
	int ret;

	snprintf(path, sizeof(path), CFGFS_CORE"/%s/%s/action/%s",
		 dev->tcm_hba_name, dev->tcm_dev_name, name);
	LOG_DEBUG("dev: `, executing action `", dev->tcm_dev_name, name);
	ret = tcmu_cfgfs_set_u32(path, val);
	LOG_DEBUG("dev: `, action ` done", dev->tcm_dev_name, name);
	return ret;
}
