#ifndef __KERNEL__

#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>

#else

#define pr_fmt(fmt) KBUILD_MODNAME " :%s:%d:" fmt, __func__, __LINE__
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/vfs.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <asm/syscalls.h>
#include <linux/uio.h>
#define UINT64_MAX 18446744073709551615ULL

#endif

#include "lsmt_ro_file.h"
#include "function.h"

const static uint64_t MAX_OFFSET = (1UL << 50) - 1;
const static uint32_t MAX_LENGTH = (1 << 14) - 1;
//const static uint64_t INVALID_OFFSET = MAX_OFFSET;
const static uint64_t INVALID_OFFSET = (1UL << 50) - 1;
const static uint32_t ALIGNMENT4K = 4 << 10;
//const static uint32_t ALIGNMENT      = 512U;
#define ALIGNMENT 512U
const static int MAX_LAYERS = 255;
const static int MAX_IO_SIZE = 4 * 1024 * 1024;

/* ============================== Segments ================================= */
static struct segment_mapping INVALID_MAPPING = { (1UL << 50) - 1, 0, 0, 0, 0 };

void print_segment(const struct segment *m)
{
	PRINT_INFO("[ offset: %lu, length: %u ]\n", m->offset, m->length);
}

void print_segment_mapping(const struct segment_mapping *m)
{
	PRINT_INFO("[ offset: %lu, length: %u, moffset: %lu,"
		   " zeroed: %u, tag: %u ]\n",
		   m->offset, m->length, m->moffset, m->zeroed, m->tag);
}

static uint64_t segment_end(const void /* const struct segment */ *m)
{
	const struct segment *s = (const struct segment *)m;
	return s->offset + s->length;
}

static uint64_t segment_mapping_mend(const struct segment_mapping *m)
{
	if (m->zeroed)
		return m->moffset;
	return m->moffset + m->length;
}

// It seems unnecessary...
// void discard(struct segment_mapping *m)
// {
//         m->zeroed = 1;
// }

static bool verify_mapping_order(const struct segment_mapping *pmappings,
				 size_t n)
{
	if (n < 2)
		return true;
	const struct segment_mapping *it = pmappings;
	for (; it != pmappings + n - 1; it++) {
		const struct segment_mapping *nt = it + 1;
		if (segment_end(it) <= nt->offset)
			continue;
		PRINT_ERROR("segment disordered. [%llu %llu] , [%llu %llu]",
			    it->offset, segment_end(it), nt->offset,
			    segment_end(nt));
		return false;
	}
	return true;
}

static bool verify_mapping_moffset(const struct segment_mapping *pmappings,
				   size_t n, uint64_t moffset_begin,
				   uint64_t moffset_end)
{
	const struct segment_mapping *it = pmappings;
	for (; it != pmappings + n; it++) {
		if (it->zeroed == 1) {
			if ((moffset_begin <= it->moffset) &&
			    (segment_mapping_mend(it) <= moffset_end)) {
				continue;
			}
			return false;
		}
		if (!(moffset_begin <= it->moffset &&
		      segment_mapping_mend(it) <= moffset_end &&
		      it->moffset < segment_mapping_mend(it))) {
			PRINT_ERROR("invalid index moffset: "
				    "[ %llu, %llu ] not in [%llu, %llu]",
				    it->moffset, segment_mapping_mend(it),
				    moffset_begin, moffset_end);
			return false;
		}
	}
	return true;
}

void forward_offset_to(void *m, uint64_t x, int8_t type)
{
	struct segment *s = (struct segment *)m;
	print_segment(s);
	PRINT_INFO("x is %llu\n", x);
	ASSERT(x >= s->offset);
	uint64_t delta = x - s->offset;
	s->offset = x;
	s->length -= delta;
	if (type == TYPE_SEGMENT_MAPPING) {
		struct segment_mapping *tmp = (struct segment_mapping *)m;
		if (!tmp->zeroed) {
			tmp->moffset += delta;
		}
	}
}

void backward_end_to(void *m, uint64_t x)
{
	struct segment *s = (struct segment *)m;
	if (x <= s->offset) {
		print_segment(s);
		PRINT_ERROR("%llu > %llu is FALSE", x, s->offset);
	}
	ASSERT(x > s->offset);
	s->length = x - s->offset;
}

static void trim_edge(void *m, const struct segment *bound_segment,
		      uint8_t type)
{
	if (((struct segment *)m)->offset < bound_segment->offset) {
		print_segment((struct segment *)m);
		PRINT_INFO("From trim_edge to forward_offset_to\n");
		PRINT_INFO("bound_segment->offset is %llu\n",
			   bound_segment->offset);
		forward_offset_to(m, bound_segment->offset, type);
	}
	if (segment_end(m) > segment_end(bound_segment)) {
		backward_end_to(m, segment_end(bound_segment));
	}
}

/* =============================== Index =================================== */
size_t ro_index_size(const struct lsmt_ro_index *index)
{
	return index->pend - index->pbegin;
}

struct lsmt_ro_index *
create_memory_index(const struct segment_mapping *pmappings, size_t n,
		    uint64_t moffset_begin, uint64_t moffset_end, bool copy)
{
	bool ok0 = verify_mapping_order(pmappings, n);
	bool ok1 = verify_mapping_moffset(pmappings, n, moffset_begin,
					  moffset_end);
	struct lsmt_ro_index *ret = NULL;
	if (ok0 & ok1) {
		int index_size = sizeof(struct lsmt_ro_index);
		if (copy) {
			index_size += sizeof(struct lsmt_ro_index) * n;
		}
#ifndef __KERNEL__
		ret = (struct lsmt_ro_index *)malloc(index_size);
#else
		ret = (struct lsmt_ro_index *)kvmalloc(index_size, GFP_KERNEL);
#endif
		if (!ret) {
#ifndef __KERNEL__
			PRINT_ERROR("malloc memory failed. %d %s", errno,
				    strerror(errno));
#else
			PRINT_ERROR("malloc memory failed with error code %d",
				    ret);
#endif
			return NULL;
		}
		if (!copy) {
			ret->pbegin = pmappings;
			ret->pend = pmappings + n;
		} else {
			memcpy(ret->mapping, pmappings,
			       n * sizeof(struct segment_mapping));
			ret->pbegin = ret->mapping;
			ret->pend = ret->mapping + n;
		}
		return ret;
	}
	return NULL;
};

const struct segment_mapping *
ro_index_lower_bound(const struct lsmt_ro_index *index, uint64_t offset)
{
	const struct segment_mapping *l = index->pbegin;
	const struct segment_mapping *r = index->pend - 1;
	int ret = -1;
	while (l <= r) {
		int m = ((l - index->pbegin) + (r - index->pbegin)) >> 1;
		const struct segment_mapping *cmp = index->pbegin + m;
		if (offset >= segment_end(cmp)) {
			ret = m;
			l = index->pbegin + (m + 1);
		} else {
			r = index->pbegin + (m - 1);
		}
	}
	const struct segment_mapping *pret = index->pbegin + (ret + 1);
	if (pret >= index->pend) {
		return index->pend;
	} else {
		return pret;
	}
}

int ro_index_lookup(const struct lsmt_ro_index *index,
		    const struct segment *query_segment,
		    struct segment_mapping *ret_mappings, size_t n)
{
	if (query_segment->length == 0)
		return 0;
	const struct segment_mapping *lb =
		ro_index_lower_bound(index, query_segment->offset);
	int cnt = 0;
	const struct segment_mapping *it = lb;
	for (; it != index->pend; it++) {
		if (it->offset >= segment_end(query_segment))
			break;
		ret_mappings[cnt++] = *it;
		if (cnt == n)
			break;
	}
	if (cnt == 0)
		return 0;
	trim_edge(&ret_mappings[0], query_segment, TYPE_SEGMENT_MAPPING);
	if (cnt > 1) {
		trim_edge(&ret_mappings[cnt - 1], query_segment,
			  TYPE_SEGMENT_MAPPING);
	}
	return cnt;
}

/* ========================= HeaderTrailer ============================= */
static const uint32_t FLAG_SHIFT_HEADER = 0; // 1:header     0:trailer
static const uint32_t FLAG_SHIFT_TYPE = 1; // 1:data file, 0:index file
static const uint32_t FLAG_SHIFT_SEALED = 2; // 1:YES,       0:NO
static const uint32_t HT_SPACE = 4096;

struct _UUID {
	uint32_t a;
	uint16_t b, c, d;
	uint8_t e[6];
};

static uint64_t *MAGIC0 = (uint64_t *)"LSMT\0\1\2";

static struct _UUID MAGIC1 = { 0xd2637e65,
			       0x4494,
			       0x4c08,
			       0xd2a2,
			       { 0xc8, 0xec, 0x4f, 0xcf, 0xae, 0x8a } };

struct lsmt_ht {
	uint64_t magic0;
	struct _UUID magic1;
	// offset 24, 28
	uint32_t size; //= sizeof(HeaderTrailer);
	uint32_t flags; //= 0;
	// offset 32, 40, 48
	uint64_t index_offset; // in bytes
	uint64_t index_size; // # of SegmentMappings
	uint64_t virtual_size; // in bytes
} __attribute__((packed));

static uint32_t get_flag_bit(const struct lsmt_ht *ht, uint32_t shift)
{
	return ht->flags & (1 << shift);
}

static bool is_header(const struct lsmt_ht *ht)
{
	return get_flag_bit(ht, FLAG_SHIFT_HEADER);
}
static bool is_trailer(const struct lsmt_ht *ht)
{
	return !is_header(ht);
}
static bool is_data_file(const struct lsmt_ht *ht)
{
	return get_flag_bit(ht, FLAG_SHIFT_TYPE);
}
static bool is_index_file(const struct lsmt_ht *ht)
{
	return !is_data_file(ht);
}
static bool is_sealed(const struct lsmt_ht *ht)
{
	return get_flag_bit(ht, FLAG_SHIFT_SEALED);
}
static bool verify_magic(const struct lsmt_ht *ht)
{
	return ht->magic0 == *MAGIC0 &&
	       (memcmp(&ht->magic1, &MAGIC1, sizeof(MAGIC1)) == 0);
}

/* ========================= LSMTReadOnly File ============================= */

int set_max_io_size(struct lsmt_ro_file *file, size_t size)
{
	if (size == 0 || (size & (ALIGNMENT4K - 1)) != 0) {
		PRINT_ERROR("size( %ld ) is not aligned with 4K.", size);
		return -1;
	}
	file->MAX_IO_SIZE = size;
	return 0;
}

size_t get_max_io_size(const struct lsmt_ro_file *file)
{
	return file->MAX_IO_SIZE;
}

static struct segment_mapping *
do_load_index(void *fd, struct lsmt_ht *pheader_tail, bool trailer, ssize_t *n)
{
	size_t index_size = 0;
	struct lsmt_ht *pht = NULL;
	ALIGNED_MEM(buf, HT_SPACE, ALIGNMENT4K);
	size_t file_size = 0;
	struct segment_mapping *ibuf = NULL;
	struct segment_mapping *ret_index = NULL;
	uint64_t index_bytes;
	pr_info("here ma i \n");
	int ret = _lsmt_pread(fd, buf, HT_SPACE, 0);
	pr_info("here ma i \n");

	if (ret < (ssize_t)HT_SPACE) {
		PRINT_ERROR("failed to read file header (fildes: %d).",
			    *(int *)fd);
		goto error_ret;
	}

	pht = (struct lsmt_ht *)buf;
	if (!verify_magic(pht) || !is_header(pht))
		goto error_ret;
	file_size = _lsmt_get_file_size(fd);
	if (trailer) {
		size_t trailer_offset = file_size - HT_SPACE;
		if (!is_data_file(pht)) {
			PRINT_ERROR("uncognized file type (fildes: %d).",
				    *(int *)fd);
			goto error_ret;
		}
		pr_info("here ma i \n");
		ret = _lsmt_pread(fd, buf, HT_SPACE, trailer_offset);
		pr_info("here ma i \n");
		if (ret < (ssize_t)HT_SPACE) {
			PRINT_ERROR("failed to read file trailer "
				    "(fildes: %d).",
				    (int)(uint64_t)fd);
			goto error_ret;
		}
		if (!verify_magic(pht) || !is_trailer(pht) ||
		    !is_data_file(pht) || !is_sealed(pht)) {
			PRINT_ERROR("trailer magic, trailer type, "
				    "file type or sealedness doesn't match"
				    " (fides: %d. %d)",
				    (int)(uint64_t)fd, is_trailer(pht));
			goto error_ret;
		}
		index_bytes = pht->index_size * sizeof(struct segment_mapping);
		if (index_bytes > trailer_offset - pht->index_offset) {
			PRINT_ERROR("invalid index bytes or size "
				    "(fildes: %d).",
				    (int)(uint64_t)fd);
			goto error_ret;
		}
	} else {
		if (!is_index_file(pht) || is_sealed(pht)) {
			PRINT_ERROR("file type or sealedness wrong "
				    "(fildes: %d).",
				    (int)(uint64_t)fd);
			goto error_ret;
		}
		if (pht->index_offset != HT_SPACE) {
			PRINT_ERROR("index offset wrong (fildes: %d)",
				    (int)(uint64_t)fd);
			goto error_ret;
		}

		index_bytes = file_size - HT_SPACE;
		pht->index_size = index_bytes / sizeof(struct segment_mapping);
	}

#ifndef __KERNEL__
	posix_memalign((void **)&ibuf, ALIGNMENT4K,
		       pht->index_size * sizeof(*ibuf));
#else
	ibuf = (struct segment_mapping *)kvmalloc(
		pht->index_size * sizeof(*ibuf), GFP_KERNEL);
#endif
	pr_info("here ma i \n");
	ret = _lsmt_pread(fd, ibuf, index_bytes, pht->index_offset);
	pr_info("here ma i ret is %d, index_bytes is %d \n,", ret, index_bytes);
	//从file的 HeaderTrailer::SPACE 偏移开始读入index
	if (ret < (ssize_t)index_bytes) {
		_lsmt_free(ibuf);
		PRINT_ERROR("failed to read index (fildes: %d).",
			    (int)(uint64_t)fd);
		goto error_ret;
	}
	size_t i;
	for (i = 0; i < pht->index_size; ++i) {
		if (ibuf[i].offset == INVALID_OFFSET)
			continue;
		ibuf[index_size] = ibuf[i];
		ibuf[index_size].tag = 0;
		index_size++;
	}
	pht->index_size = index_size;
	if (pheader_tail)
		*pheader_tail = *pht;
	ret_index = (struct segment_mapping *)_lsmt_malloc(
		index_size * sizeof(struct segment_mapping));

	memcpy(ret_index, ibuf, index_size * sizeof(*ret_index));
	*n = index_size;
	_lsmt_free(ibuf);
	return ret_index;
error_ret:
#ifndef __KERNEL__
	PRINT_ERROR("errno: %d, msg: %s", errno, strerror(errno));
#else
	PRINT_ERROR("ret is %d", ret);
#endif
	return NULL;
}

struct lsmt_ro_file *open_file(void *fd, bool ownership)
{
	struct lsmt_ro_file *rst = NULL;
	struct lsmt_ro_index *pi = NULL;
	struct segment_mapping *p = NULL;
	struct lsmt_ht ht;
	ssize_t n = 0;
	size_t rst_size = 0;
	struct segment_mapping *it = NULL;
	if (fd == NULL) {
		PRINT_ERROR("invalid file ptr. (fildes: %d)",
			    (int)(uint64_t)fd);
		goto error_ret;
	}
	p = do_load_index(fd, &ht, true, &n);

	if (!p) {
#ifndef __KERNEL__
		errno = EIO;
#endif
		PRINT_ERROR("failed to load index from file (fildes: %d).",
			    (int)(uint64_t)fd);
		goto error_ret;
	}
	//将索引p的地址返回给pi
	pi = create_memory_index(p, ht.index_size, HT_SPACE / ALIGNMENT,
				 ht.index_offset / ALIGNMENT, false);
	if (!pi) {
		PRINT_ERROR("failed to create memory index (fildes: %d).",
			    (int)(uint64_t)fd);
		goto error_ret;
	}
	it = (struct segment_mapping *)pi->pbegin;
	for (; it != pi->pend; it++) {
		it->tag++;
	}
	rst_size = sizeof(struct lsmt_ro_file) + 2 * sizeof(int);
	rst = (struct lsmt_ro_file *)_lsmt_malloc(rst_size);

	if (!rst) {
		PRINT_ERROR("failed to malloc memory. (size: %lu)", rst_size);
	}
	rst->m_index = pi;
	rst->m_files[0] = NULL;
	rst->m_files[1] = fd;
	rst->m_files_count = 2;
	rst->m_vsize = ht.virtual_size;
	rst->m_ownership = ownership;
	rst->MAX_IO_SIZE = MAX_IO_SIZE;
	return rst;

error_ret:
	return NULL;
}

int close_file(struct lsmt_ro_file **file)
{
	PRINT_INFO("destruct file. addr: %llx", (uint64_t)*file);
	if (*file == NULL)
		return 0;
	bool ok = true;
	if ((*file)->m_ownership) {
		int i;
		for (i = 0; i < (int)((*file)->m_files_count); i++) {
			if ((*file)->m_files[i] != NULL) {
#ifndef __KERNEL__
				int fd = (int)(uint64_t)((*file)->m_files[i]);
				PRINT_INFO("close file, fildes: %d", fd);
				if (close(fd) == 0)
					continue;
				PRINT_ERROR("close file error. (fildes: %d, "
					    "errno: %d, msg: %s",
					    fd, errno, strerror(errno));
#else
				struct file *filep =
					(struct file *)((*file)->m_files[i]);
				if (filp_close(filep, NULL) == 0)
					continue;
				PRINT_ERROR("close file error. ");
#endif
				ok = false;
			}
		}
	}
	if (!ok)
		return -1;
	PRINT_INFO("free memory. addr: %llx", (uint64_t)*file);
	_lsmt_free(*file);
	*file = NULL;
	return 0;
}

static int merge_indexes(int level, struct lsmt_ro_index **indexes, size_t n,
			 struct segment_mapping *mappings[], size_t *size,
			 size_t *capacity, uint64_t start, uint64_t end)
{
	if (level >= n)
		return 0;
	PRINT_INFO("level %d range [ %llu, %llu ] %lu", level, start, end,
		   ro_index_size(indexes[level]));
	struct segment_mapping *p =
		(struct segment_mapping *)ro_index_lower_bound(indexes[level],
							       start);
	const struct segment_mapping *pend = indexes[level]->pend;
	if (p == pend) {
		merge_indexes(level + 1, indexes, n, mappings, size, capacity,
			      start, end);
		return 0;
	}
	struct segment_mapping it = *p;
	if (start > it.offset) {
		forward_offset_to(&it, start, TYPE_SEGMENT_MAPPING);
	}

	while (p != pend) {
		if (end <= it.offset)
			break;
		if (start < it.offset) {
			merge_indexes(level + 1, indexes, n, mappings, size,
				      capacity, start, it.offset);
		}
		if (end < segment_end(&it)) {
			backward_end_to(&it, end);
		}
		if (*size == *capacity) {
			size_t tmp = (*capacity) << 1;
			PRINT_INFO("realloc array. ( %lu -> %lu )", *capacity,
				   tmp);
#ifndef __KERNEL__
			struct segment_mapping *m =
				(struct segment_mapping *)realloc(
					*mappings,
					tmp * sizeof(struct segment_mapping));
			if (m == NULL) {
				PRINT_ERROR(
					"realloc failed. errno: %d, msg: %s",
					errno, strerror(errno));
				return -1;
			}
#else
			struct segment_mapping *m =
				(struct segment_mapping *)krealloc(
					*mappings,
					tmp * sizeof(struct segment_mapping),
					GFP_KERNEL);
			if (m == NULL) {
				PRINT_ERROR("realloc failed. ");
				return -1;
			}
#endif

			*mappings = m;
			*capacity = tmp;
		}

		it.tag = level;
		(*mappings)[*size] = it;
		(*size)++;
		start = segment_end(p);
		p++;
		it = *p;
	}
	if (start < end) {
		merge_indexes(level + 1, indexes, n, mappings, size, capacity,
			      start, end);
	}
	return 0;
}

static struct lsmt_ro_index *
merge_memory_indexes(struct lsmt_ro_index **indexes, size_t n)
{
	size_t size = 0;
	size_t capacity = ro_index_size(indexes[0]);
	struct lsmt_ro_index *ret = NULL;
	struct segment_mapping *tmp = NULL;
	struct segment_mapping *mappings =
		(struct segment_mapping *)_lsmt_malloc(
			sizeof(struct segment_mapping) * capacity);
	if (!mappings)
		goto err_ret;

	merge_indexes(0, indexes, n, &mappings, &size, &capacity, 0,
		      UINT64_MAX);
	PRINT_INFO("merge done, index size: %lu", size);

	ret = (struct lsmt_ro_index *)_lsmt_malloc(
		sizeof(struct lsmt_ro_index));
	tmp = (struct segment_mapping *)_lsmt_realloc(
		mappings, size * sizeof(struct segment_mapping));

	if (!tmp || !ret)
		goto err_ret;
	ret->pbegin = tmp;
	ret->pend = tmp + size;
	PRINT_INFO("ret index done. size: %lu", size);
	return ret;

err_ret:
	_lsmt_free(mappings);
	_lsmt_free(ret);
	_lsmt_free(tmp);
	return NULL;
}

static struct lsmt_ro_index *load_merge_index(void **files, size_t n,
					      struct lsmt_ht *ht)
{
	struct lsmt_ro_index *indexes[MAX_LAYERS];
	struct lsmt_ro_index *pmi = NULL;
	if (n > MAX_LAYERS) {
		PRINT_ERROR("too many indexes to merge, %d at most!",
			    MAX_LAYERS);
		return NULL;
	}
	int i;
	for (i = 0; i < n; ++i) {
		ssize_t size = 0;
		pr_info("here ma i \n");
		struct segment_mapping *p =
			do_load_index(files[i], ht, true, &size);
		pr_info("here ma i \n");
		if (!p) {
			PRINT_ERROR("failed to load index from %d-th file", i);
#ifndef __KERNEL__
			errno = EIO;
#endif
			return NULL;
		}
		pr_info("here ma i \n");
		struct lsmt_ro_index *pi = create_memory_index(
			p, ht->index_size, HT_SPACE / ALIGNMENT,
			ht->index_offset / ALIGNMENT, false);
		pr_info("here ma i \n");
		if (!pi) {
			PRINT_ERROR("failed to create memory index! "
				    "( %d-th file )",
				    i);
			_lsmt_free(p);
			return NULL;
		}
		indexes[i] = pi;
	}

	REVERSE_LIST(int, (int *)&files[0], (int *)&files[n - 1]);
	REVERSE_LIST(struct lsmt_ro_index *, &indexes[0], &indexes[n - 1]);

	pr_info("here ma i \n");
	pmi = merge_memory_indexes(&indexes[0], n);
	pr_info("here ma i \n");

	if (!pmi) {
		PRINT_ERROR("failed to merge indexes %s", "");
		goto error_ret;
	}
	return pmi;

error_ret:
	return NULL;
}

#if 0
size_t lsmt_iter_read(struct lsmt_ro_file *file, 
	struct iov_iter * iter, loff_t *ppos, int type)
{
	//ssize_t ret = 0;
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, flags);
	if (ret)
		return ret;
	kiocb.ki_pos = *ppos;

	if(type==READ)


}
#endif

//this function peers to vfs_iter_read
size_t lsmt_iter_read(struct lsmt_ro_file *file, struct iov_iter *iter,
		      loff_t *ppos, int type)
{
	ssize_t ret = 0;
	ASSERT(type == 0); //only do read
	//iov_iter from loop device --> buf and offset in lsmt --> iov_iter to use in vfs_iter_read
	size_t readn = 0;
	int NMAPPING = 16;
	char *data = NULL;
	size_t nbytes = 0;
	if (uaccess_kernel()) {
		struct kvec *kv;
		kv = iter->kvec;
		data = kv->iov_base;
		nbytes = iter->kvec->iov_len;
	} else {
		data = iter->iov->iov_base;
		nbytes = iter->iov->iov_len;
	}

	pr_info("initial pos is %x, size is ", *ppos);

	off_t offset = *ppos;
	struct segment_mapping mapping[NMAPPING];
	struct segment s = { (uint64_t)offset / ALIGNMENT,
			     (uint32_t)nbytes / ALIGNMENT };

	while (true) {
		int n = ro_index_lookup(file->m_index, &s, mapping, NMAPPING);
		int i;
		for (i = 0; i < n; i++) {
			//fill in unmapped area
			if (s.offset < mapping[i].offset) {
				size_t length = (mapping[i].offset - s.offset) *
						ALIGNMENT;
				memset((void *)data, 0, length);
				data += length;
				readn += length;
			}
			void *fd = file->m_files[mapping[i].tag];
			ssize_t size = mapping[i].length * ALIGNMENT;
			ssize_t read = 0;
			//now init iov_iter for vfs_iter_read
			struct iovec iov = { .iov_base = data,
					     .iov_len = size };
			loff_t pos = mapping[i].moffset * ALIGNMENT;
			//init with new iov
			iov_iter_init(iter, READ, &iov, 1, size);
			if (mapping[i].zeroed == 0) {
				pr_info("Before do vfs_iter_read pos is %x, size is %x",
					pos, size);
				read = vfs_iter_read(fd, iter, &pos, 0);
				if (read < size) {
#ifndef __KERNEL__
					PRINT_ERROR(
						"read %d-th file error."
						"(%ld < %ld) errno: %d msg: %s",
						mapping[i].tag, read, size,
						errno, strerror(errno));
#else
					PRINT_ERROR("read %d-th file error."
						    "(%ld < %ld) Read is %d",
						    mapping[i].tag, read, size,
						    read);
#endif
					return -1;
				}
			} else {
				read = size;
				memset(data, 0, size);
			}
			readn += read;
			data += size;
			forward_offset_to(&s, segment_end(&mapping[i]),
					  TYPE_SEGMENT);
		}
		if (n < NMAPPING)
			break;
	}
	if (s.length > 0) {
		size_t length = s.length * ALIGNMENT;
		memset(data, 0, length);
		data += length;
		readn += length;
	}
	return readn;
}

size_t lsmt_pread_try(struct lsmt_ro_file *file, void *buf, size_t nbytes,
		      loff_t *poffset)
{
	size_t readn = 0;
	int NMAPPING = 16;
	char *data = (char *)buf;

	struct segment_mapping mapping[NMAPPING];
	if ((nbytes | *poffset) & (ALIGNMENT - 1)) {
		PRINT_ERROR("count(%lu) and offset(%llu) must be aligned",
			    nbytes, *poffset);
		//exit(0);
		return -1;
	}
	while (nbytes > file->MAX_IO_SIZE) {
		size_t read =
			lsmt_pread(file, data, file->MAX_IO_SIZE, *poffset);
		if (read < file->MAX_IO_SIZE) {
			PRINT_ERROR("read data error: (return %lu < %lu )",
				    read, file->MAX_IO_SIZE);
			return -1;
		}
		data += read;
		nbytes -= read;
		readn += read;
	}

	struct segment s = { (uint64_t)*poffset / ALIGNMENT,
			     (uint32_t)nbytes / ALIGNMENT };

	while (true) {
		int n = ro_index_lookup(file->m_index, &s, mapping, NMAPPING);
		int i;
		for (i = 0; i < n; i++) {
			if (s.offset < mapping[i].offset) {
				size_t length = (mapping[i].offset - s.offset) *
						ALIGNMENT;
				memset((void *)data, 0, length);
				data += length;
				readn += length;
			}
			void *fd = file->m_files[mapping[i].tag];
			ssize_t size = mapping[i].length * ALIGNMENT;
			ssize_t read = 0;
			if (mapping[i].zeroed == 0) {
				read = _lsmt_pread(fd, data, size,
						   mapping[i].moffset *
							   ALIGNMENT);
				if (read < size) {
#ifndef __KERNEL__
					PRINT_ERROR(
						"read %d-th file error."
						"(%ld < %ld) errno: %d msg: %s",
						mapping[i].tag, read, size,
						errno, strerror(errno));
#else
					PRINT_ERROR("read %d-th file error."
						    "(%ld < %ld) Read is %d",
						    mapping[i].tag, read, size,
						    read);
#endif
					return -1;
				}
			} else {
				read = size;
				memset(data, 0, size);
			}
			readn += read;
			data += size;
			//*poffset += read;
			forward_offset_to(&s, segment_end(&mapping[i]),
					  TYPE_SEGMENT);
		}
		if (n < NMAPPING)
			break;
	}
	if (s.length > 0) {
		size_t length = s.length * ALIGNMENT;
		memset(data, 0, length);
		data += length;
		readn += length;
	}
	*poffset += readn;
	return readn;
}

size_t lsmt_pread(struct lsmt_ro_file *file, void *buf, size_t nbytes,
		  off_t offset)
{
	size_t readn = 0;
	int NMAPPING = 16;
	char *data = (char *)buf;
	struct segment_mapping mapping[NMAPPING];
	if ((nbytes | offset) & (ALIGNMENT - 1)) {
		PRINT_ERROR("count(%lu) and offset(%llu) must be aligned",
			    nbytes, offset);
		//exit(0);
		return -1;
	}
	while (nbytes > file->MAX_IO_SIZE) {
		size_t read = lsmt_pread(file, data, file->MAX_IO_SIZE, offset);
		if (read < file->MAX_IO_SIZE) {
			PRINT_ERROR("read data error: (return %lu < %lu )",
				    read, file->MAX_IO_SIZE);
			return -1;
		}
		data += read;
		offset += read;
		nbytes -= read;
		readn += read;
	}

	struct segment s = { (uint64_t)offset / ALIGNMENT,
			     (uint32_t)nbytes / ALIGNMENT };

	while (true) {
		int n = ro_index_lookup(file->m_index, &s, mapping, NMAPPING);
		int i;
		for (i = 0; i < n; i++) {
			if (s.offset < mapping[i].offset) {
				size_t length = (mapping[i].offset - s.offset) *
						ALIGNMENT;
				memset((void *)data, 0, length);
				data += length;
				readn += length;
			}
			void *fd = file->m_files[mapping[i].tag];
			ssize_t size = mapping[i].length * ALIGNMENT;
			ssize_t read = 0;
			if (mapping[i].zeroed == 0) {
				read = _lsmt_pread(fd, data, size,
						   mapping[i].moffset *
							   ALIGNMENT);
				if (read < size) {
#ifndef __KERNEL__
					PRINT_ERROR(
						"read %d-th file error."
						"(%ld < %ld) errno: %d msg: %s",
						mapping[i].tag, read, size,
						errno, strerror(errno));
#else
					PRINT_ERROR("read %d-th file error."
						    "(%ld < %ld) Read is %d",
						    mapping[i].tag, read, size,
						    read);
#endif
					return -1;
				}
			} else {
				read = size;
				memset(data, 0, size);
			}
			readn += read;
			data += size;
			forward_offset_to(&s, segment_end(&mapping[i]),
					  TYPE_SEGMENT);
		}
		if (n < NMAPPING)
			break;
	}
	if (s.length > 0) {
		size_t length = s.length * ALIGNMENT;
		memset(data, 0, length);
		data += length;
		readn += length;
	}
	return readn;
}

struct lsmt_ro_file *open_files(void **files, size_t n, bool ownership)
{
	pr_info("here ma i \n");
	struct lsmt_ro_file *ret = (struct lsmt_ro_file *)_lsmt_malloc(
		sizeof(files[0]) * n + sizeof(struct lsmt_ro_file));
	pr_info("here ma i \n");

	struct lsmt_ht ht;
	struct lsmt_ro_index *idx = load_merge_index(files, n, &ht);
	if (idx == NULL) {
		return NULL;
	}
	pr_info("here ma i \n");
	ret->m_files_count = n;
	ret->m_index = idx;
	ret->m_ownership = ownership;
	ret->m_vsize = ht.virtual_size;
	ret->MAX_IO_SIZE = MAX_IO_SIZE;
	pr_info("here ma i \n");
	memcpy(ret->m_files, &files[0], n * sizeof(files[0]));
	pr_info("here ma i \n");
	return ret;
}
