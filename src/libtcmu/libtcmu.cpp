/*
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

#include <memory.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <scsi/scsi.h>

#include <libnl3/netlink/genl/genl.h>
#include <libnl3/netlink/genl/mngt.h>
#include <libnl3/netlink/genl/ctrl.h>
#include <libnl3/netlink/errno.h>

#include "target_core_user_local.h"
#include "libtcmu.h"
#include "libtcmu_priv.h"
#include "scsi_defs.h"

#include <photon/common/alog.h>
#include <photon/thread/thread.h>

#define TCMU_NL_VERSION 2

static struct nla_policy tcmu_attr_policy[TCMU_ATTR_MAX+1] = {
	{},
	{ .type = NLA_STRING },
	{ .type = NLA_U32 },
	{},
	{ .type = NLA_STRING },
	{ .type = NLA_U64 },
	{ .type = NLA_U8 },
	{ .type = NLA_S32 },
	{ .type = NLA_U32 },
	{ .type = NLA_U8 }
};

static int device_add(struct tcmulib_context *ctx, char *dev_name,
		      char *cfgstring, bool reopen);
static int device_remove(struct tcmulib_context *ctx, char *dev_name,
			 bool should_block);
static int handle_netlink(struct nl_cache_ops *unused, struct genl_cmd *cmd,
			  struct genl_info *info, void *arg);

static struct genl_cmd tcmu_cmds[] = {
	{
		.c_id		= TCMU_CMD_ADDED_DEVICE,
		.c_name		= "ADDED DEVICE",
		.c_maxattr	= TCMU_ATTR_MAX,
		.c_msg_parser	= handle_netlink,
		.c_attr_policy	= tcmu_attr_policy,
	},
	{
		.c_id		= TCMU_CMD_REMOVED_DEVICE,
		.c_name		= "REMOVED DEVICE",
		.c_maxattr	= TCMU_ATTR_MAX,
		.c_msg_parser	= handle_netlink,
		.c_attr_policy	= tcmu_attr_policy,
	},
	{
		.c_id		= TCMU_CMD_RECONFIG_DEVICE,
		.c_name		= "RECONFIG DEVICE",
		.c_maxattr	= TCMU_ATTR_MAX,
		.c_msg_parser	= handle_netlink,
		.c_attr_policy	= tcmu_attr_policy,
	},
};

static struct genl_ops tcmu_ops = {
	.o_hdrsize  = 0,
	.o_id       = 0,
	.o_name		= "TCM-USER",
	.o_cache_ops = nullptr,
	.o_cmds		= tcmu_cmds,
	.o_ncmds	= ARRAY_SIZE(tcmu_cmds),
};

static int send_netlink_reply(struct tcmulib_context *ctx, int reply_cmd,
			      uint32_t dev_id, int status)
{
	struct nl_sock *sock = ctx->nl_sock;
	struct nl_msg *msg;
	void *hdr;
	int ret = -ENOMEM;

	msg = nlmsg_alloc();
	if (!msg)
		return ret;

	hdr = genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, tcmu_ops.o_id,
			  0, 0, reply_cmd, TCMU_NL_VERSION);
	if (!hdr)
		goto free_msg;

	ret = nla_put_s32(msg, TCMU_ATTR_CMD_STATUS, status);
	if (ret < 0)
		goto free_msg;

	ret = nla_put_u32(msg, TCMU_ATTR_DEVICE_ID, dev_id);
	if (ret < 0)
		goto free_msg;

	/* Ignore ack. There is nothing we can do. */
	ret = nl_send_auto(sock, msg);
free_msg:
	nlmsg_free(msg);

	if (ret < 0)
		LOG_ERROR("Could not send netlink cmd `", reply_cmd);
	return ret;
}

static struct tcmu_device *
lookup_dev_by_name(struct tcmulib_context *ctx, char *dev_name)
{
	auto it = ctx->devices.find(dev_name);
	if (it == ctx->devices.end()) {
		return NULL;
	}
	return it->second;
}

static const char *const tcmulib_cfg_type_lookup[] = {
	[TCMULIB_CFG_DEV_CFGSTR]  = "TCMULIB_CFG_DEV_CFGSTR",
	[TCMULIB_CFG_DEV_SIZE]    = "TCMULIB_CFG_DEV_SIZE",
	[TCMULIB_CFG_WRITE_CACHE] = "TCMULIB_CFG_WRITE_CACHE",
};

static int reconfig_device(struct tcmulib_context *ctx, char *dev_name,
			   struct genl_info *info)
{
	struct tcmu_device *dev;
	struct tcmulib_cfg_info cfg;
	int ret;

	memset(&cfg, 0, sizeof(cfg));

	dev = lookup_dev_by_name(ctx, dev_name);
	if (!dev) {
		LOG_DEBUG("skipping reconfig for device `: not managed by us",
			 dev_name);
		return -ENODATA;
	}

	if (info->attrs[TCMU_ATTR_DEV_CFG]) {
		cfg.type = TCMULIB_CFG_DEV_CFGSTR;
		cfg.data.dev_cfgstring =
				nla_get_string(info->attrs[TCMU_ATTR_DEV_CFG]);
	} else if (info->attrs[TCMU_ATTR_DEV_SIZE]) {
		cfg.type = TCMULIB_CFG_DEV_SIZE;
		cfg.data.dev_size = nla_get_u64(info->attrs[TCMU_ATTR_DEV_SIZE]);
	} else if (info->attrs[TCMU_ATTR_WRITECACHE]) {
		cfg.type = TCMULIB_CFG_WRITE_CACHE;
		cfg.data.write_cache =
				nla_get_u8(info->attrs[TCMU_ATTR_WRITECACHE]);
	} else {
		LOG_ERROR("[dev `] Unknown reconfig attr. Try updating libtcmu.", dev->tcm_dev_name);
		return -EOPNOTSUPP;
	}

	if (!dev->handler->reconfig) {
		LOG_DEBUG("[dev `] Reconfiguration is not supported with this device. Request for `",
			dev->tcm_dev_name, tcmulib_cfg_type_lookup[cfg.type]);
		return -EOPNOTSUPP;
	}

	ret = dev->handler->reconfig(dev, &cfg);
	if (ret < 0) {
		LOG_DEBUG("[dev `] Handler reconfig for ` failed with error `",
		            dev->tcm_dev_name, tcmulib_cfg_type_lookup[cfg.type], strerror(-ret));
		return ret;
	}

	return 0;
}

static int handle_netlink(struct nl_cache_ops *unused, struct genl_cmd *cmd,
			  struct genl_info *info, void *arg)
{
	struct tcmulib_context *ctx = arg;
	int ret, reply_cmd, version = info->genlhdr->version;
	char buf[32];


	LOG_DEBUG("cmd `. Got header version `. Supported `",
		 cmd->c_id, info->genlhdr->version, TCMU_NL_VERSION);

	if (!info->attrs[TCMU_ATTR_MINOR] || !info->attrs[TCMU_ATTR_DEVICE]) {
		LOG_ERROR("TCMU_ATTR_MINOR or TCMU_ATTR_DEVICE not set, dropping netlink command.");
		return 0;
	}

	if (version > 1 && !info->attrs[TCMU_ATTR_DEVICE_ID]) {
		LOG_ERROR("TCMU_ATTR_DEVICE_ID not set in v` cmd `, dropping netink command.", version, cmd->c_id);
		return 0;
	}

	snprintf(buf, sizeof(buf), "uio%d", nla_get_u32(info->attrs[TCMU_ATTR_MINOR]));

	switch (cmd->c_id) {
	case TCMU_CMD_ADDED_DEVICE:
		reply_cmd = TCMU_CMD_ADDED_DEVICE_DONE;
		ret = device_add(ctx, buf,
				 nla_get_string(info->attrs[TCMU_ATTR_DEVICE]),
				 false);
		break;
	case TCMU_CMD_REMOVED_DEVICE:
		reply_cmd = TCMU_CMD_REMOVED_DEVICE_DONE;
		ret = device_remove(ctx, buf, false);
		break;
	case TCMU_CMD_RECONFIG_DEVICE:
		reply_cmd = TCMU_CMD_RECONFIG_DEVICE_DONE;
		ret = reconfig_device(ctx, buf, info);
		break;
	default:
		LOG_ERROR("Unknown netlink command `. Netlink header received version `. libtcmu supports `",
			 cmd->c_id, version, TCMU_NL_VERSION);
		return -EOPNOTSUPP;
	}

	if (version > 1 && ret != -ENODATA)
		ret = send_netlink_reply(ctx, reply_cmd,
				nla_get_u32(info->attrs[TCMU_ATTR_DEVICE_ID]),
				ret);

	return ret;
}

static int set_genl_features(struct nl_sock *sock)
{
	struct nl_msg *msg;
	void *hdr;
	int ret = -NLE_NOMEM;

	msg = nlmsg_alloc();
	if (!msg) {
		LOG_ERROR("Could not allocate a new message.");
		return ret;
	}

	hdr = genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, tcmu_ops.o_id,
			  0, NLM_F_ACK, TCMU_CMD_SET_FEATURES, TCMU_NL_VERSION);
	if (!hdr)
		goto free_msg;

	ret = nla_put_u8(msg, TCMU_ATTR_SUPP_KERN_CMD_REPLY, 1);
	if (ret < 0)
		goto free_msg;

	/*
	 * Could be a older kernel. Ignore failure and just work in degraded
	 * mode.
	 */
	ret = nl_send_sync(sock, msg);
	if (ret == -NLE_OPNOTSUPP) {
		LOG_WARN("Kernel does not support the operation.");
		ret = 0;
	}
	goto done;

free_msg:
	nlmsg_free(msg);

done:
	if (ret < 0)
		LOG_ERROR("Could not set features. Error `", ret);

	return ret;
}

static struct nl_sock *setup_netlink(struct tcmulib_context *ctx)
{
	struct nl_sock *sock;
	int ret;

	sock = nl_socket_alloc();
	if (!sock) {
		LOG_ERROR("couldn't alloc socket");
		return NULL;
	}

	nl_socket_disable_seq_check(sock);

	nl_socket_modify_cb(sock, NL_CB_VALID, NL_CB_CUSTOM, genl_handle_msg, ctx);

	ret = genl_connect(sock);
	if (ret < 0) {
		LOG_ERROR("couldn't connect");
		goto err_free;
	}

	ret = genl_register_family(&tcmu_ops);
	if (ret < 0) {
		LOG_ERROR("couldn't register family");
		goto err_close;
	}

	ret = genl_ops_resolve(sock, &tcmu_ops);
	if (ret < 0) {
		LOG_ERROR("couldn't resolve ops, is target_core_user.ko loaded?");
		goto err_unregister;
	}

	ret = genl_ctrl_resolve_grp(sock, "TCM-USER", "config");
	if (ret < 0) {
		LOG_ERROR("couldn't resolve netlink family group, is target_core_user.ko loaded?");
		goto err_unregister;
	}

	ret = nl_socket_add_membership(sock, ret);
	if (ret < 0) {
		LOG_ERROR("couldn't add membership");
		goto err_unregister;
	}

	ret = set_genl_features(sock);
	if (ret < 0)
		goto err_unregister;

	return sock;

err_unregister:
	genl_unregister_family(&tcmu_ops);
err_close:
	nl_close(sock);
err_free:
	nl_socket_free(sock);

	return NULL;
}

static void teardown_netlink(struct nl_sock *sock)
{
	int ret;

	ret = genl_unregister_family(&tcmu_ops);
	if (ret != 0) {
		LOG_ERROR("genl_unregister_family failed, `", ret);
	}

	nl_close(sock);
	nl_socket_free(sock);
}

static struct tcmulib_handler *find_handler(struct tcmulib_context *ctx,
					    char *cfgstring)
{
	size_t len;
	char *found_at;

	found_at = strchrnul(cfgstring, '/');
	len = found_at - cfgstring;

	for (size_t i = 0; i < ctx->handlers.size(); i++) {
		if (strlen(ctx->handlers[i].subtype) == len &&
		    !strncmp(cfgstring, ctx->handlers[i].subtype, len)) {
			LOG_INFO("find handler for subtype: `", cfgstring);
			return &ctx->handlers[i];
		}
	}
	return NULL;
}

void tcmu_dev_flush_ring(struct tcmu_device *dev)
{
	struct tcmu_mailbox *mb = dev->map;

	LOG_DEBUG("[dev `] waiting for ring to clear", dev->tcm_dev_name);
	while (mb->cmd_head != mb->cmd_tail)
		photon::thread_usleep(50000);
	LOG_DEBUG("[dev `] ring clear", dev->tcm_dev_name);
}

bool tcmu_dev_oooc_supported(struct tcmu_device* dev)
{
	return dev->map->flags & TCMU_MAILBOX_FLAG_CAP_OOOC;
}

/* Read a size_t from a file.  Returns -1 on error. */
static ssize_t read_size(const char *filename)
{
	int fd;
	int len, rc;
	char buf[256], *endbuf;
	ssize_t ret;

	fd = open(filename, O_RDONLY);
	if (fd == -1)
		goto err;

	len = read(fd, buf, sizeof(buf)-1);
	rc = close(fd);
	if (len <= 0 || rc == -1)
		goto err;

	buf[len] = '\0'; /* null-terminate */
	ret = strtoull(buf, &endbuf, 0);
	if (buf == endbuf || (size_t)ret == ULLONG_MAX)
		goto err;

	return ret;

err:
	LOG_WARN("cannot read size from `", filename);
	return -1;
}

/* Extract configuration parameters into dev. */
static bool device_parse_cfg(struct tcmu_device *dev,
			     const char *dev_name, const char *cfgstring)
{
	int len;
	const char *ptr, *oldptr;

	len = snprintf(dev->dev_name, sizeof(dev->dev_name), "%s", dev_name);
	if (len >= (int)sizeof(dev->dev_name)) {
		LOG_ERROR("device name too long for tcmu_device");
		goto err_recompile;
	}

	/* Check valid cfgstring */
	oldptr = cfgstring;
	ptr = strchr(oldptr, '/');
	if (!ptr)
		goto err_badcfg;
	if (strncmp(cfgstring, "tcm-user", ptr-oldptr))
		goto err_badcfg;

	/* Get HBA name */
	oldptr = ptr+1;
	ptr = strchr(oldptr, '/');
	if (!ptr)
		goto err_badcfg;
	len = ptr-oldptr;
	len = snprintf(dev->tcm_hba_name, sizeof(dev->tcm_hba_name), "user_%.*s", len, oldptr);
	if (len >= (int)sizeof(dev->tcm_hba_name)) {
		LOG_ERROR("hba name too long for tcmu_device");
		goto err_recompile;
	}

	/* Get device name */
	oldptr = ptr+1;
	ptr = strchr(oldptr, '/');
	if (!ptr)
		goto err_badcfg;
	len = ptr-oldptr;
	len = snprintf(dev->tcm_dev_name, sizeof(dev->tcm_dev_name), "%.*s", len, oldptr);
	if (len >= (int)sizeof(dev->tcm_dev_name)) {
		LOG_ERROR("tcm device name too long for tcmu_device");
		goto err_recompile;
	}

	/* The rest is the handler-specific cfgstring */
	oldptr = ptr+1;
	len = snprintf(dev->cfgstring, sizeof(dev->cfgstring), "%s", oldptr);
	if (len >= (int)sizeof(dev->cfgstring)) {
		LOG_WARN("additional handler cfgstring was truncated");
		/* not a terminal error. snprintf() will null-terminate */
	}
	return true;

err_badcfg:
	LOG_ERROR("invalid cfgstring: expecting \"tcm-user/<hba_name>/<tcm_device_name>/<handler_name_config>\"");
err_recompile: /* consider expanding string lengths in dev */
	return false;
}

static void device_close_shm(struct tcmu_device *dev)
{
	int ret;

	ret = close(dev->fd);
	if (ret != 0) {
		LOG_ERROR("could not close device fd for `: `", dev->dev_name, errno);
	}
	ret = munmap(dev->map, dev->map_len);
	if (ret != 0) {
		LOG_ERROR("could not unmap device `: `", dev->dev_name, errno);
	}
}

static bool device_open_shm(struct tcmu_device *dev)
{
	size_t mmap_size;
	char *mmap_name;
	off_t mmap_offset;

	/* get filename, size and offset */
	mmap_name = tcmu_dev_get_memory_info(dev, NULL, &mmap_size, &mmap_offset);
	if (!mmap_name)
		goto err_fail;

	/* cache the map size */
	dev->map_len = mmap_size;

	/* open the map */
	dev->fd = open(mmap_name, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (dev->fd == -1) {
		LOG_ERROR("could not open `, errno: `", mmap_name, errno);
		goto err_mmap_name;
	}

	/* bring the map into memory */
	dev->map = mmap(NULL, dev->map_len, PROT_READ|PROT_WRITE, MAP_SHARED, dev->fd, mmap_offset);
	if (dev->map == MAP_FAILED) {
		LOG_ERROR("could not mmap `, mmap len: `, mmap off: `, fd: `, errno: `",
			mmap_name, dev->map_len, mmap_offset, dev->fd, errno);
		goto err_fd_close;
	}

	if (dev->map->version != KERN_IFACE_VER) {
		LOG_ERROR("Kernel interface version mismatch: wanted ` got `",
			KERN_IFACE_VER, (int)dev->map->version);
		goto err_munmap;
	}

	free(mmap_name);
	return true;

err_munmap:
	munmap(dev->map, dev->map_len);
err_fd_close:
	close(dev->fd);
err_mmap_name:
	free(mmap_name);
err_fail:
	return false;
}

static void device_free(struct tcmu_device *dev)
{
	delete dev;
}

static int device_add(struct tcmulib_context *ctx, char *dev_name,
		      char *cfgstring, bool reopen)
{
	struct tcmu_device *dev;
	char *reason = NULL;
	int rc;
	bool reset_supp = true;

	dev = new tcmu_device();
	if (!dev) {
		LOG_ERROR("calloc failed for device_add()");
		return -ENOMEM;
	}
	if (!device_parse_cfg(dev, dev_name, cfgstring))
		goto err_free;

	dev->handler = find_handler(ctx, dev->cfgstring);
	if (!dev->handler) {
		LOG_WARN("skipping device `: no matching handler for subtype",
			 dev->dev_name);
		device_free(dev);
		return -ENODATA;
	}

	if (dev->handler->check_config &&
	    !dev->handler->check_config(dev->cfgstring, &reason)) {
		/* It may be handled by other handlers */
		LOG_ERROR("check_config failed for ` because of `", dev->dev_name, reason);
		free(reason);
		goto err_free;
	}

	if (reopen) {
		/*
		 * We might not have cleanly shutdown and IO might be
		 * running in the kernel or have timed out. Block the device
		 * so new IO is stopped, and reset the ring so we can start
		 * from a fresh slate. We will unblock below when we are
		 * completely setup.
		 */
		rc = tcmu_cfgfs_dev_exec_action(dev, "block_dev", 1);
		/*
		 * As long as the block_dev file existed, try to reset
		 * just in case the kernel was in a invald state.
		 */
		if (rc == -ENOENT) {
			reset_supp = false;
		} else {
			/*
			 * Force a retry of the outstanding commands.
			 */
			rc = tcmu_cfgfs_dev_exec_action(dev, "reset_ring", 1);
			if (rc)
				LOG_ERROR("[dev `] Could not reset ring `", dev->tcm_dev_name, rc);
		}
	}

	if (!device_open_shm(dev))
		goto err_unblock;

	dev->cmd_tail = dev->map->cmd_tail;
	dev->ctx = ctx;

	rc = dev->handler->added(dev);
	if (rc != 0) {
		LOG_ERROR("handler open failed for `", dev->dev_name);
		goto err_closeshm;
	}
	ctx->devices[dev->dev_name] = dev;

	if (reopen && reset_supp)
		tcmu_cfgfs_dev_exec_action(dev, "block_dev", 0);

	return 0;

err_closeshm:
	device_close_shm(dev);
err_unblock:
	if (reopen && reset_supp)
		tcmu_cfgfs_dev_exec_action(dev, "block_dev", 0);
err_free:
	device_free(dev);
	return -ENOENT;
}

static void close_devices(struct tcmulib_context *ctx)
{
	auto devices = ctx->devices;
	for (auto dev : devices) {
		device_remove(ctx, dev.second->dev_name, true);
	}
}

static int device_remove(struct tcmulib_context *ctx, char *dev_name,
			 bool should_block)
{
	struct tcmu_device *dev;

	dev = lookup_dev_by_name(ctx, dev_name);
	if (!dev) {
		LOG_WARN("skipping remove for device `: not managed by us",
			 dev_name);
		return -ENODATA;
	}

	/*
	 * If called through nl, IO will be stopped. If called by a
	 * app/daemon, IO might be runnning. Try to do a ordered
	 * shutdown and allow IO to complete normally.
	 */
	if (should_block) {
		tcmu_cfgfs_dev_exec_action(dev, "block_dev", 1);
		tcmu_dev_flush_ring(dev);
	}

	ctx->devices.erase(dev_name);

	dev->handler->removed(dev);

	device_close_shm(dev);

	if (should_block)
		tcmu_cfgfs_dev_exec_action(dev, "block_dev", 0);

	LOG_DEBUG("[dev `] removed from tcmulib", dev->tcm_dev_name);
	device_free(dev);
	return 0;
}

static int read_uio_name(const char *uio_dev, char **dev_name)
{
	int fd;
	char *tmp_path;
	int ret = -1;
	char buf[PATH_MAX] = {'\0'};

	if (asprintf(&tmp_path, "/sys/class/uio/%s/name", uio_dev) == -1)
		return -1;

	fd = open(tmp_path, O_RDONLY);
	if (fd == -1) {
		LOG_ERROR("could not open `", tmp_path);
		goto free_path;
	}

	ret = read(fd, buf, sizeof(buf));
	if (ret <= 0 || ret >= (int)sizeof(buf)) {
		LOG_ERROR("read of ` had issues", tmp_path);
		goto close;
	}

	buf[ret-1] = '\0'; /* null-terminate and chop off the \n */

	*dev_name = strdup(buf);

	ret = 0;

close:
	close(fd);
free_path:
	free(tmp_path);
	return ret;
}

static int is_uio(const struct dirent *dirent)
{
	char *dev_name = NULL;
	ssize_t ret = 0;

	if (strncmp(dirent->d_name, "uio", 3))
		return 0;

	if (read_uio_name(dirent->d_name, &dev_name))
		goto out;

	/* we only want uio devices whose name is a format we expect */
	if (strncmp(dev_name, "tcm-user", 8))
		goto out;

	ret = 1;

out:
	if (dev_name)
		free(dev_name);
	return ret;
}

static int open_devices(struct tcmulib_context *ctx)
{
	struct dirent **dirent_list;
	int num_devs;
	int num_good_devs = 0;
	int i;

	num_devs = scandir("/dev", &dirent_list, is_uio, alphasort);
	if (num_devs == -1)
		return -1;

	for (i = 0; i < num_devs; i++) {
		char *dev_name = NULL;

		if (read_uio_name(dirent_list[i]->d_name, &dev_name))
			continue;

		if (device_add(ctx, dirent_list[i]->d_name, dev_name, true) < 0) {
			free(dev_name);
			continue;
		}
		free(dev_name);

		num_good_devs++;
	}

	for (i = 0; i < num_devs; i++)
		free(dirent_list[i]);
	free(dirent_list);

	return num_good_devs;
}

static void release_resources(struct tcmulib_context *ctx)
{
	teardown_netlink(ctx->nl_sock);
	delete ctx;
}

struct tcmulib_context *tcmulib_initialize(std::vector<struct tcmulib_handler> &handlers)
{
	struct tcmulib_context *ctx = new(struct tcmulib_context);
	if (!ctx)
		return NULL;

	ctx->nl_sock = setup_netlink(ctx);
	if (!ctx->nl_sock) {
		delete ctx;
		return NULL;
	}
	ctx->handlers = std::move(handlers);

	int ret = open_devices(ctx);
	if (ret < 0) {
		release_resources(ctx);
		return NULL;
	}

	return ctx;
}

void tcmulib_close(struct tcmulib_context *ctx)
{
	close_devices(ctx);
	release_resources(ctx);
}

int tcmulib_get_master_fd(struct tcmulib_context *ctx)
{
	return nl_socket_get_fd(ctx->nl_sock);
}

int tcmulib_master_fd_ready(struct tcmulib_context *ctx)
{
	return nl_recvmsgs_default(ctx->nl_sock);
}

void *tcmu_dev_get_private(struct tcmu_device *dev)
{
	return dev->hm_private;
}

void tcmu_dev_set_private(struct tcmu_device *dev, void *pri)
{
	dev->hm_private = pri;
}

const char *tcmu_dev_get_uio_name(struct tcmu_device *dev)
{
	return dev->dev_name;
}

void tcmu_set_thread_name(const char *prefix, struct tcmu_device *dev)
{
	const char *uio = dev ? tcmu_dev_get_uio_name(dev) : NULL;
	char *pname;

	/*
	 * If we are trying to set the pthread name in the
	 * event work thread, we must ignore it.
	 */
	if (__tcmu_is_ework_thread) {
		LOG_WARN("[dev `] Do not set name for event work thread in the callback fn", dev->tcm_dev_name);
		return;
	}

	if (!prefix) {
		LOG_ERROR("[dev `] Failed to set name for thread `", dev->tcm_dev_name,
			     (long unsigned int)pthread_self());
		return;
	}

	if (asprintf(&pname, "%s%s%s", prefix, uio ? "-" : "",  uio ? uio : "") == -1) {
		LOG_ERROR("[dev `] Could not allocate thread name", dev->tcm_dev_name);
		return;
	}

	if (strlen(pname) >= TCMU_THREAD_NAME_LEN) {
		LOG_WARN("[dev `] Cannot set thread name to `. Name must be less than ` chars.",
			      dev->tcm_dev_name, pname, TCMU_THREAD_NAME_LEN);
		pname[TCMU_THREAD_NAME_LEN - 1] = '\0';
		LOG_WARN("[dev `] Truncating to `", dev->tcm_dev_name, pname);
	}

	if (pthread_setname_np(pthread_self(), pname))
		LOG_ERROR("[dev `] Could not set thread name to `", dev->tcm_dev_name, pname);
	free(pname);
}

void tcmu_dev_set_num_lbas(struct tcmu_device *dev, uint64_t num_lbas)
{
	dev->num_lbas = num_lbas;
}

uint64_t tcmu_dev_get_num_lbas(struct tcmu_device *dev)
{
	return dev->num_lbas;
}

uint64_t tcmu_lba_to_byte(struct tcmu_device *dev, uint64_t lba)
{
	return lba << dev->block_size_shift;
}

uint64_t tcmu_byte_to_lba(struct tcmu_device *dev, uint64_t byte)
{
	return byte >> dev->block_size_shift;
}

uint64_t tcmu_cdb_to_byte(struct tcmu_device *dev, uint8_t *cdb)
{
	return tcmu_lba_to_byte(dev, tcmu_cdb_get_lba(cdb));
}

void tcmu_dev_set_block_size(struct tcmu_device *dev, uint32_t block_size)
{
	dev->block_size = block_size;
	dev->block_size_shift = ffs(block_size) - 1;
}

uint32_t tcmu_dev_get_block_size(struct tcmu_device *dev)
{
	return dev->block_size;
}

/**
 * tcmu_dev_set_max_xfer_len - set device's max command size
 * @dev: tcmu device
 * @len: max transfer length in block_size sectors
 */
void tcmu_dev_set_max_xfer_len(struct tcmu_device *dev, uint32_t len)
{
	dev->max_xfer_len = len;
}

uint32_t tcmu_dev_get_max_xfer_len(struct tcmu_device *dev)
{
	return dev->max_xfer_len;
}

/**
 * tcmu_dev_set_opt_xcopy_rw_len - set device's emulated xcopy chunk len
 * @dev: tcmu device
 * @len: optimal RW len, in block_size sectors, for emulate xcopy operations
 */
void tcmu_dev_set_opt_xcopy_rw_len(struct tcmu_device *dev, uint32_t len)
{
	dev->opt_xcopy_rw_len = len;
}

uint32_t tcmu_dev_get_opt_xcopy_rw_len(struct tcmu_device *dev)
{
	return dev->opt_xcopy_rw_len;
}

/**
 * tcmu_dev_set/get_opt_unmap_gran - set/get device's optimal unmap granularity
 * @dev: tcmu device
 * @len: optimal unmap granularity length in block_size sectors
 * @split: true if handler needs unmaps larger then len to be split for it.
 */
void tcmu_dev_set_opt_unmap_gran(struct tcmu_device *dev, uint32_t len,
				 bool split)
{
	dev->split_unmaps = split;
	dev->opt_unmap_gran = len;
}

uint32_t tcmu_dev_get_opt_unmap_gran(struct tcmu_device *dev)
{
	return dev->opt_unmap_gran;
}

/**
 * tcmu_dev_set/get_max_unmap_len - set/get device's man unmap len
 * @dev: tcmu device
 * @len: max unmap len in block_size sectors
 */
void tcmu_dev_set_max_unmap_len(struct tcmu_device *dev, uint32_t len)
{
	dev->max_unmap_len = len;
}

uint32_t tcmu_dev_get_max_unmap_len(struct tcmu_device *dev)
{
	return dev->max_unmap_len;
}

/**
 * tcmu_dev_set/get_unmap_gran_align - set/get device's unmap granularity alignment
 * @dev: tcmu device
 * @len: unmap granularity alignment length in block_size sectors
 */
void tcmu_dev_set_unmap_gran_align(struct tcmu_device *dev, uint32_t len)
{
	dev->unmap_gran_align = len;
}

uint32_t tcmu_dev_get_unmap_gran_align(struct tcmu_device *dev)
{
	return dev->unmap_gran_align;
}

void tcmu_dev_set_write_cache_enabled(struct tcmu_device *dev, bool enabled)
{
	dev->write_cache_enabled = enabled;
}

bool tcmu_dev_get_write_cache_enabled(struct tcmu_device *dev)
{
	return dev->write_cache_enabled;
}

void tcmu_dev_set_solid_state_media(struct tcmu_device *dev, bool solid_state)
{
	dev->solid_state_media = solid_state;
}

bool tcmu_dev_get_solid_state_media(struct tcmu_device *dev)
{
	return dev->solid_state_media;
}

void tcmu_dev_set_unmap_enabled(struct tcmu_device *dev, bool enabled)
{
	dev->unmap_enabled = enabled;
}

bool tcmu_dev_get_unmap_enabled(struct tcmu_device *dev)
{
	return dev->unmap_enabled;
}

void tcmu_dev_set_write_protect_enabled(struct tcmu_device *dev, bool enabled)
{
	dev->write_protect_enabled = enabled;
}

bool tcmu_dev_get_write_protect_enabled(struct tcmu_device *dev)
{
	return dev->write_protect_enabled;
}

int tcmu_dev_get_fd(struct tcmu_device *dev)
{
	return dev->fd;
}

/**
 * tcmu_dev_get_memory_info - retrieve information about tcmu's shared
 * memory block.  If the memory is mapped to another address (e.g., in
 * another process) then the information in struct iovec can be converted
 * using these values.  Returns the filename of the mmap() that fd uses, or
 * NULL if information is not available.  The caller must free() the
 * returned filename.
 * @dev: tcmu device
 * @base: receives the base address of the shared memory region
 * @len: receives the size of the shared memory region
 * @offset: receives the offset within fd for mmap() (conventionally 0).
 */
char *
tcmu_dev_get_memory_info(struct tcmu_device *dev, void **base,
			 size_t *len, off_t *offset)
{
	char *mmap_name;
	const char *namefmt = "/dev/%s";
	const char *sizefmt = "/sys/class/uio/%s/maps/map0/size";

	if (asprintf(&mmap_name, namefmt, dev->dev_name) == -1) {
		LOG_ERROR("cannot construct device map filename");
		goto err_fail;
	}
	if (base)
		*base = dev->map;
	if (len) {
		if (dev->map_len != 0) {
			/* cached */
			*len = dev->map_len;
		} else {
			/* get length of map from file */
			ssize_t size;
			char *size_name;

			if (asprintf(&size_name, sizefmt, dev->dev_name) == -1) {
				LOG_ERROR("cannot construct device map size filename");
				goto err_free;
			}
			size = read_size(size_name);
			free(size_name);
			if (size == -1) {
				LOG_ERROR("unable to read device map0 size");
				goto err_free;
			}
			*len = size;
		}
	}
	if (offset)
		*offset = 0;
	return mmap_name;

err_free:
	free(mmap_name);
err_fail:
	return NULL;
}

char *tcmu_dev_get_cfgstring(struct tcmu_device *dev)
{
	return dev->cfgstring;
}

struct tcmulib_handler *tcmu_dev_get_handler(struct tcmu_device *dev)
{
	return dev->handler;
}

static inline struct tcmu_cmd_entry *
device_cmd_head(struct tcmu_device *dev)
{
	struct tcmu_mailbox *mb = dev->map;

	/*
	 * We must load the mb_head index using an atomic load or we'll crash
	 * on aarch64. See https://github.com/open-iscsi/tcmu-runner/issues/688
	 */
	uint32_t mb_head = __atomic_load_n(&mb->cmd_head, __ATOMIC_ACQUIRE);
	struct tcmu_cmd_entry* e = (struct tcmu_cmd_entry *) ((char *) mb + mb->cmdr_off + mb_head);
	return e;
}

static inline struct tcmu_cmd_entry *
device_cmd_tail(struct tcmu_device *dev)
{
	struct tcmu_mailbox *mb = dev->map;

	return (struct tcmu_cmd_entry *) ((char *) mb + mb->cmdr_off + dev->cmd_tail);
}

/* update the tcmu_device's tail */
#define TCMU_UPDATE_DEV_TAIL(dev, mb, ent) \
do { \
	dev->cmd_tail = (dev->cmd_tail + tcmu_hdr_get_len((ent)->hdr.len_op)) % mb->cmdr_size; \
} while (0)

struct tcmulib_cmd *tcmulib_get_next_command(struct tcmu_device *dev,
					     int hm_cmd_size)
{
	struct tcmu_mailbox *mb = dev->map;
	struct tcmu_cmd_entry *ent;

	while ((ent = device_cmd_tail(dev)) != device_cmd_head(dev)) {

		switch (tcmu_hdr_get_op(ent->hdr.len_op)) {
		case TCMU_OP_PAD:
			/* do nothing */
			break;
		case TCMU_OP_CMD: {
			int i;
			struct tcmulib_cmd *cmd;
			uint8_t *cdb = (uint8_t *) mb + ent->req.cdb_off;
			int cdb_len = tcmu_cdb_get_length(cdb);

			if (cdb_len < 0) {
				/*
				 * This should never happen so just drop cmd
				 * for now instead of adding a lock in the
				 * main IO path.
				 */
				break;
			}

			/* Alloc memory for cmd itself, iovec and cdb */
			cmd = malloc(sizeof(*cmd) + hm_cmd_size + cdb_len +
				     sizeof(*cmd->iovec) * ent->req.iov_cnt);
			if (!cmd)
				return NULL;
			cmd->cmd_id = ent->hdr.cmd_id;
			cmd->completion_next = nullptr;

			/* Convert iovec addrs in-place to not be offsets */
			cmd->iov_cnt = ent->req.iov_cnt;
			cmd->iovec = (struct iovec *) (cmd + 1);
			for (i = 0; i < (int)ent->req.iov_cnt; i++) {
				cmd->iovec[i].iov_base = (void *) mb +
					(size_t) ent->req.iov[i].iov_base;
				cmd->iovec[i].iov_len = ent->req.iov[i].iov_len;
			}

			/* Copy cdb that currently points to the command ring */
			cmd->cdb = (uint8_t *) (cmd->iovec + cmd->iov_cnt);
			memcpy(cmd->cdb, (void *) mb + ent->req.cdb_off, cdb_len);

			/* Setup handler memory area after iovecs and cdb */
			if (hm_cmd_size)
				cmd->hm_private = cmd->cdb + cdb_len;

			TCMU_UPDATE_DEV_TAIL(dev, mb, ent);
			return cmd;
		}
		default:
			/* We don't even know how to handle this TCMU opcode. */
			ent->hdr.uflags |= TCMU_UFLAG_UNKNOWN_OP;
		}

		TCMU_UPDATE_DEV_TAIL(dev, mb, ent);
	}

	return NULL;
}

static int tcmu_sts_to_scsi(int tcmu_sts, uint8_t *sense)
{
	switch (tcmu_sts) {
	case TCMU_STS_OK:
		return SAM_STAT_GOOD;
	case TCMU_STS_NO_RESOURCE:
		return SAM_STAT_TASK_SET_FULL;
	/*
	 * We drop the session during timeout handling so force
	 * a retry to have it handled during session level recovery.
	 */
	case TCMU_STS_TIMEOUT:
	case TCMU_STS_BUSY:
		return SAM_STAT_BUSY;
	case TCMU_STS_PASSTHROUGH_ERR:
		break;
	/* Check Conditions below */
	case TCMU_STS_RANGE:
		/* LBA out of range */
		tcmu_sense_set_data(sense, ILLEGAL_REQUEST, 0x2100);
		break;
	case TCMU_STS_HW_ERR:
		/* Internal target failure */
		tcmu_sense_set_data(sense, HARDWARE_ERROR, 0x4400);
		break;
	case TCMU_STS_MISCOMPARE:
		/* Miscompare during verify operation */
		__tcmu_sense_set_data(sense, MISCOMPARE, 0x1d00);
		break;
	case TCMU_STS_RD_ERR:
		/* Read medium error */
		tcmu_sense_set_data(sense, MEDIUM_ERROR, 0x1100);
		break;
	case TCMU_STS_WR_ERR:
		/* Write medium error */
		tcmu_sense_set_data(sense, MEDIUM_ERROR, 0x0C00);
		break;
	case TCMU_STS_INVALID_CDB:
		/* Invalid field in CDB */
		tcmu_sense_set_data(sense, ILLEGAL_REQUEST, 0x2400);
		break;
	case TCMU_STS_INVALID_PARAM_LIST:
		/* Invalid field in parameter list */
		tcmu_sense_set_data(sense, ILLEGAL_REQUEST, 0x2600);
		break;
	case TCMU_STS_INVALID_PARAM_LIST_LEN:
		/* Invalid list parameter list length */
		tcmu_sense_set_data(sense, ILLEGAL_REQUEST, 0x1a00);
		break;
	case TCMU_STS_NOTSUPP_SEG_DESC_TYPE:
		/* Unsupported segment descriptor type code */
		tcmu_sense_set_data(sense, ILLEGAL_REQUEST, 0x2609);
		break;
	case TCMU_STS_NOTSUPP_TGT_DESC_TYPE:
		/* Unsupported target descriptor type code */
		tcmu_sense_set_data(sense, ILLEGAL_REQUEST, 0x2607);
		break;
	case TCMU_STS_TOO_MANY_SEG_DESC:
		/* The number of segment descriptors exceeds the allowed number */
		tcmu_sense_set_data(sense, ILLEGAL_REQUEST, 0x2608);
		break;
	case TCMU_STS_TOO_MANY_TGT_DESC:
		/* The number of CSCD descriptors exceeds the allowed number */
		tcmu_sense_set_data(sense, ILLEGAL_REQUEST, 0x2606);
		break;
	case TCMU_STS_CP_TGT_DEV_NOTCONN:
		/* Copy target device not reachable */
		tcmu_sense_set_data(sense, COPY_ABORTED, 0x0D02);
		break;
	case TCMU_STS_INVALID_CP_TGT_DEV_TYPE:
		/* Invalid copy target device type */
		tcmu_sense_set_data(sense, COPY_ABORTED, 0x0D03);
		break;
	case TCMU_STS_CAPACITY_CHANGED:
		/* Device capacity has changed */
		tcmu_sense_set_data(sense, UNIT_ATTENTION, 0x2A09);
		break;
	case TCMU_STS_TRANSITION:
		/* ALUA state transition */
		tcmu_sense_set_data(sense, NOT_READY, 0x040A);
		break;
	case TCMU_STS_IMPL_TRANSITION_ERR:
		/* Implicit ALUA state transition failed */
		tcmu_sense_set_data(sense, UNIT_ATTENTION, 0x2A07);
		break;
	case TCMU_STS_EXPL_TRANSITION_ERR:
		/* STPG failed */
		tcmu_sense_set_data(sense, HARDWARE_ERROR, 0x670A);
		break;
	case TCMU_STS_FENCED:
		/* ALUA state in standby */
		tcmu_sense_set_data(sense, NOT_READY, 0x040B);
		break;
	case TCMU_STS_WR_ERR_INCOMPAT_FRMT:
		/* Can't write - incompatible format */
		tcmu_sense_set_data(sense, ILLEGAL_REQUEST, 0x3005);
		break;
	case TCMU_STS_NOTSUPP_SAVE_PARAMS:
		/* Saving params not supported */
		tcmu_sense_set_data(sense, ILLEGAL_REQUEST, 0x3900);
		break;
	case TCMU_STS_FRMT_IN_PROGRESS:
		/* Format in progress */
		__tcmu_sense_set_data(sense, NOT_READY, 0x0404);
		break;
	case TCMU_STS_NOT_HANDLED:
	case TCMU_STS_INVALID_CMD:
		/* Invalid op code */
		tcmu_sense_set_data(sense, ILLEGAL_REQUEST, 0x2000);
		break;
	default:
		LOG_ERROR("Invalid tcmu status code `", tcmu_sts);
		/* Fall through. Kernel will translate to LUN comm failure */
	}

	return SAM_STAT_CHECK_CONDITION;
}

/* update the ring buffer's tail */
#define TCMU_UPDATE_RB_TAIL(mb, ent) \
do { \
	mb->cmd_tail = (mb->cmd_tail + tcmu_hdr_get_len((ent)->hdr.len_op)) % mb->cmdr_size; \
} while (0)

static void command_complete_locked(struct tcmu_device *dev,
				    struct tcmulib_cmd *cmd,
				    int result)
{
	struct tcmu_mailbox *mb = dev->map;
	struct tcmu_cmd_entry *ent = (void *) mb + mb->cmdr_off + mb->cmd_tail;

	/* current command could be PAD in async case */
	while (ent != (void *) mb + mb->cmdr_off + mb->cmd_head) {
		if (tcmu_hdr_get_op(ent->hdr.len_op) == TCMU_OP_CMD)
			break;
		TCMU_UPDATE_RB_TAIL(mb, ent);
		ent = (void *) mb + mb->cmdr_off + mb->cmd_tail;
	}

	/* cmd_id could be different in async case */
	if (cmd->cmd_id != ent->hdr.cmd_id) {
		ent->hdr.cmd_id = cmd->cmd_id;
	}

	ent->rsp.scsi_status = tcmu_sts_to_scsi(result, cmd->sense_buf);
	if (ent->rsp.scsi_status == SAM_STAT_CHECK_CONDITION) {
		memcpy(ent->rsp.sense_buffer, cmd->sense_buf,
		       TCMU_SENSE_BUFFERSIZE);
	}

	TCMU_UPDATE_RB_TAIL(mb, ent);
	free(cmd);
}

void tcmulib_command_complete(
		struct tcmu_device *dev,
		struct tcmulib_cmd *cmd,
		int result)
{
	photon::scoped_lock lock(dev->completion_lock);
	command_complete_locked(dev, cmd, result);
}

void tcmulib_processing_start(struct tcmu_device *dev)
{
	int r;
	uint32_t buf;

	/* Clear the event on the fd */
	do {
		r = read(dev->fd, &buf, 4);
	} while (r == -1 && errno == EINTR);
	if (r == -1 && errno != EAGAIN)
		LOG_ERROR("failed to read device /dev/`, `",
			 dev->dev_name, errno);
}

void tcmulib_processing_complete(struct tcmu_device *dev)
{
	int r;
	uint32_t buf = 0;

	/*
	 * Only the caller changing the counter from zero to one becomes the
	 * notifier. Other concurrent completions are merged into its next write.
	 * The original algorithm relied on cooperative threads on one VCPU; atomic
	 * operations preserve the same no-lost-wakeup property across WorkPool
	 * VCPUs without serializing the notification syscall on completion_lock.
	 */
	if (dev->aio_pending_wakeups.fetch_add(
			1, std::memory_order_acq_rel) != 0)
		return;

	for (;;) {
		/* Tell the kernel there are completed commands */
		do {
			r = write(dev->fd, &buf, 4);
		} while (r == -1 && errno == EINTR);
		if (r == -1 && errno != EAGAIN)
			LOG_ERROR("failed to write device /dev/`, `",
				 dev->dev_name, errno);

		uint32_t expected = 1;
		if (dev->aio_pending_wakeups.compare_exchange_strong(
				expected, 0, std::memory_order_acq_rel,
				std::memory_order_acquire))
			return;

		/* Consume all accumulated completions but retain notifier ownership. */
		dev->aio_pending_wakeups.exchange(1, std::memory_order_acq_rel);
	}
}
