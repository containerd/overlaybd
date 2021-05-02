#ifndef __LSMT_RO_H__
#define __LSMT_RO_H__

#ifndef HBDEBUG
#define HBDEBUG (1)
#endif

#ifndef __KERNEL__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define PRINT_INFO(fmt, ...)                                                   \
	printf("\033[33m|INFO |\033[0mline: %d|%s: " fmt "\n", __LINE__,       \
	       __FUNCTION__, __VA_ARGS__)

#define PRINT_ERROR(fmt, ...)                                                  \
	fprintf(stderr, "\033[31m|ERROR|\033[0m%s:%d|%s: " fmt "\n", __FILE__, \
		__LINE__, __FUNCTION__, __VA_ARGS__)

#define ASSERT(exp) assert(exp)
#else
#include <linux/err.h>
#include <linux/printk.h>
#define PRINT_INFO(fmt, ...)                                                   \
	do {                                                                   \
		if ((1))                                                       \
			printk(KERN_INFO fmt, ##__VA_ARGS__);                  \
	} while (0)
#define PRINT_ERROR(fmt, ...)                                                  \
	do {                                                                   \
		if ((1))                                                       \
			printk(KERN_ERR fmt, ##__VA_ARGS__);                   \
	} while (0)
//#define ASSERT(exp)						\
//	assert(exp)
#define ASSERT(x)                                                              \
	do {                                                                   \
		if (x)                                                         \
			break;                                                 \
		BUG_ON(1);                                                     \
	} while (0)

#endif

#define ALIGNED_MEM(name, size, alignment)                                     \
	char __buf##name[(size) + (alignment)];                                \
	char *name = (char *)(((uint64_t)(__buf##name + (alignment)-1)) &      \
			      ~((uint64_t)(alignment)-1));

#define REVERSE_LIST(type, begin, back)                                        \
	{                                                                      \
		type *l = (begin);                                             \
		type *r = (back);                                              \
		while (l < r) {                                                \
			type tmp = *l;                                         \
			*l = *r;                                               \
			*r = tmp;                                              \
			l++;                                                   \
			r--;                                                   \
		}                                                              \
	}

#define TYPE_SEGMENT 0
#define TYPE_SEGMENT_MAPPING 1
#define TYPE_FILDES 2
#define TYPE_LSMT_RO_INDEX 3

struct segment { /* 8 bytes */
	uint64_t offset : 50; // offset (0.5 PB if in sector)
	uint32_t length : 14; // length (8MB if in sector)
} __attribute__((packed));

struct segment_mapping { /* 8 + 8 bytes */
	uint64_t offset : 50; // offset (0.5 PB if in sector)
	uint32_t length : 14;
	uint64_t moffset : 55; // mapped offset (2^64 B if in sector)
	uint32_t zeroed : 1; // indicating a zero-filled segment
	uint8_t tag;
};

struct lsmt_ro_index {
	const struct segment_mapping *pbegin;
	const struct segment_mapping *pend;
	struct segment_mapping mapping[0];
};

struct lsmt_ro_file {
	struct lsmt_ro_index *m_index;
	uint64_t m_vsize;
	bool m_ownership;
	size_t m_files_count;
	size_t MAX_IO_SIZE;
	void *m_files[0];
};

int set_max_io_size(struct lsmt_ro_file *file, size_t size);
size_t get_max_io_size(const struct lsmt_ro_file *file);

// open a lsmt layer
struct lsmt_ro_file *open_file(void *fd, bool ownership);

// open multi LSMT layers
struct lsmt_ro_file *open_files(void **files, size_t n, bool ownership);

size_t lsmt_pread(struct lsmt_ro_file *file, void *buf, size_t nbytes,
		  off_t offset);
size_t lsmt_pread_try(struct lsmt_ro_file *file, void *buf, size_t nbytes,
		      loff_t *poffset);

size_t lsmt_iter_read(struct lsmt_ro_file *file, struct iov_iter *iter,
		      loff_t *ppos, int type);

#endif
