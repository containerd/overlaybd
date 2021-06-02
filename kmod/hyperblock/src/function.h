#ifndef __KERNEL__

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#else

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/slab.h>
//#include <linux/export.h>
#include <linux/vfs.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#endif

size_t _lsmt_get_file_size(void *fd)
{
#ifndef __KERNEL__
	struct stat stat;
	fstat((int)(uint64_t)fd, (struct stat *)stat);
	return stat.st_size;
#else
	//note that fd is pointer to struct file here
	return (size_t)i_size_read(file_inode(fd));
#endif
}

int _lsmt_fstat(void *fd, void *stat)
{
#ifndef __KERNEL__
	return fstat((int)(uint64_t)fd, (struct stat *)stat);
#else
	return vfs_getattr(&((struct file *)fd)->f_path, (struct kstat *)stat,
			   STATX_INO, AT_STATX_SYNC_AS_STAT);

#endif
}

ssize_t _lsmt_pread(void *fd, void *buf, size_t n, off_t offset)
{
#ifndef __KERNEL__
	return pread((int)(uint64_t)fd, buf, n, offset);
#else
#if LINUX_VERSION_CODE <= KERNEL_VERSION(4, 14, 14)
	mm_segment_t oldfs;
	int ret;
	oldfs = get_fs();
	set_fs(get_ds());
	ret = vfs_read((struct file *)fd, buf, n, (loff_t *)&offset);
	set_fs(oldfs);
#else
	int ret = kernel_read((struct file *)fd, buf, n, (loff_t *)&offset);
#endif
	return ret;
#endif
}

void *_lsmt_malloc(size_t size)
{
#ifndef __KERNEL__
	return malloc(size);
#else
	return kvmalloc(size, GFP_KERNEL);
#endif
}

void *_lsmt_realloc(void *ptr, size_t size)
{
#ifndef __KERNEL__
	return realloc(ptr, size);
#else
	return krealloc(ptr, size, GFP_KERNEL);
#endif
}

void _lsmt_free(void *ptr)
{
#ifndef __KERNEL__
	free(ptr);
#else
	kfree(ptr);
#endif
}
