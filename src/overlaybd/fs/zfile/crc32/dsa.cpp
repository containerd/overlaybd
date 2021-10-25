// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights reserved. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/vfio.h>
#include "dsa.h"

#define DSA_COMPL_RING_SIZE 64

unsigned int ms_timeout = 5000;
int debug_logging;
static int umwait_support;

static inline void cpuid(unsigned int *eax, unsigned int *ebx,
		unsigned int *ecx, unsigned int *edx)
{
	/* ecx is often an input as well as an output. */
	asm volatile("cpuid"
		: "=a" (*eax),
		"=b" (*ebx),
		"=c" (*ecx),
		"=d" (*edx)
		: "0" (*eax), "2" (*ecx)
		: "memory");
}

struct dsa_context *dsa_init(void)
{
	struct dsa_context *dctx;
	unsigned int unused[2];
	unsigned int leaf, waitpkg;
	int rc;
	struct accfg_ctx *ctx;

	/* detect umwait support */
	leaf = 7;
	waitpkg = 0;
	cpuid(&leaf, unused, &waitpkg, unused+1);
	if (waitpkg & 0x20) {
		dbg("umwait supported\n");
		umwait_support = 1;
	}

	dctx = (struct dsa_context *)malloc(sizeof(struct dsa_context));
	if (!dctx)
		return NULL;
	memset(dctx, 0, sizeof(struct dsa_context));

	rc = accfg_new(&ctx);
	if (rc < 0) {
		free(dctx);
		return NULL;
	}

	dctx->ctx = ctx;
	return dctx;
}

static int dsa_setup_wq(struct dsa_context *ctx, struct accfg_wq *wq)
{
	char path[PATH_MAX];
	int rc;

	rc = accfg_wq_get_user_dev_path(wq, path, PATH_MAX);
	if (rc) {
		fprintf(stderr, "Error getting uacce device path\n");
		return rc;
	}

	ctx->fd = open(path, O_RDWR);
	if (ctx->fd < 0) {
		perror("open");
		return -errno;
	}

	ctx->wq_reg = mmap(NULL, 0x1000, PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, ctx->fd, 0);
	if (ctx->wq_reg == MAP_FAILED) {
		perror("mmap");
		return -errno;
	}

	return 0;
}

static struct accfg_wq *dsa_get_wq(struct dsa_context *ctx,
		int dev_id, int shared)
{
	struct accfg_device *device;
	struct accfg_wq *wq;
	int rc;

	accfg_device_foreach(ctx->ctx, device) {
		enum accfg_device_state dstate;

		/* Make sure that the device is enabled */
		dstate = accfg_device_get_state(device);
		if (dstate != ACCFG_DEVICE_ENABLED)
			continue;

		/* Match the device to the id requested */
		if (accfg_device_get_id(device) != dev_id &&
				dev_id != -1)
			continue;

		accfg_wq_foreach(device, wq) {
			enum accfg_wq_state wstate;
			enum accfg_wq_mode mode;
			enum accfg_wq_type type;

			/* Get a workqueue that's enabled */
			wstate = accfg_wq_get_state(wq);
			if (wstate != ACCFG_WQ_ENABLED)
				continue;

			/* The wq type should be user */
			type = accfg_wq_get_type(wq);
			if (type != ACCFG_WQT_USER)
				continue;

			/* Make sure the mode is correct */
			mode = accfg_wq_get_mode(wq);
			if ((mode == ACCFG_WQ_SHARED && !shared)
				|| (mode == ACCFG_WQ_DEDICATED && shared))
				continue;

			rc = dsa_setup_wq(ctx, wq);
			if (rc < 0)
				return NULL;

			return wq;
		}
	}

	return NULL;
}

static uint32_t bsr(uint32_t val)
{
	uint32_t msb;

	msb = (val == 0) ? 0 : 32 - __builtin_clz(val);
	return msb - 1;
}

int dsa_alloc(struct dsa_context *ctx, int shared)
{
	struct accfg_device *dev;

	/* Is wq already allocated? */
	if (ctx->wq_reg)
		return 0;

	ctx->wq = dsa_get_wq(ctx, -1, shared);
	if (!ctx->wq) {
		err("No usable wq found\n");
		return -ENODEV;
	}
	dev = accfg_wq_get_device(ctx->wq);

	ctx->dedicated = !shared;
	ctx->wq_size = accfg_wq_get_size(ctx->wq);
	ctx->wq_idx = accfg_wq_get_id(ctx->wq);
	ctx->bof = accfg_wq_get_block_on_fault(ctx->wq);
	ctx->wq_max_batch_size = accfg_wq_get_max_batch_size(ctx->wq);
	ctx->wq_max_xfer_size = accfg_wq_get_max_transfer_size(ctx->wq);
	ctx->ats_disable = accfg_wq_get_ats_disable(ctx->wq);

	ctx->max_batch_size = accfg_device_get_max_batch_size(dev);
	ctx->max_xfer_size = accfg_device_get_max_transfer_size(dev);
	ctx->max_xfer_bits = bsr(ctx->max_xfer_size);

	info("alloc wq %d shared %d size %d addr %p batch sz %#x xfer sz %#x\n",
			ctx->wq_idx, shared, ctx->wq_size, ctx->wq_reg,
			ctx->max_batch_size, ctx->max_xfer_size);

	return 0;
}

int alloc_task(struct dsa_context *ctx)
{
	ctx->single_task = __alloc_task();
	if (!ctx->single_task)
		return -ENOMEM;

	dbg("single task allocated, desc %#lx comp %#lx\n",
			ctx->single_task->desc, ctx->single_task->comp);

	return DSA_STATUS_OK;
}

struct task *__alloc_task(void)
{
	struct task *tsk;

	tsk = (struct task *)malloc(sizeof(struct task));
	if (!tsk)
		return NULL;
	memset(tsk, 0, sizeof(struct task));

	tsk->desc = (struct dsa_hw_desc *)malloc(sizeof(struct dsa_hw_desc));
	if (!tsk->desc) {
		free_task(tsk);
		return NULL;
	}
	memset(tsk->desc, 0, sizeof(struct dsa_hw_desc));

	/* completion record need to be 32bits aligned */
	tsk->comp = (struct dsa_completion_record *)aligned_alloc(32, sizeof(struct dsa_completion_record));
	if (!tsk->comp) {
		free_task(tsk);
		return NULL;
	}
	memset(tsk->comp, 0, sizeof(struct dsa_completion_record));

	return tsk;
}

/* this function is re-used by batch task */
int init_task(struct task *tsk, int tflags, int opcode,
		const uint8_t *data, unsigned long xfer_size)
{
	dbg("initilizing single task %#lx\n", tsk);

	tsk->pattern = 0x0123456789abcdef;
	tsk->opcode = opcode;
	tsk->test_flags = tflags;
	tsk->xfer_size = xfer_size;

	/* allocate memory: src1*/
	switch (opcode) {
	case DSA_OPCODE_CRCGEN:
	case DSA_OPCODE_COPY_CRC:
		tsk->src1 = (void *)data;
		if (!tsk->src1)
			return -ENOMEM;
	}

	/* allocate memory: dst1*/
	switch (opcode) {
	case DSA_OPCODE_COPY_CRC:
		/* DUALCAST: dst1/dst2 lower 12 bits must be same */
		tsk->dst1 = aligned_alloc(1<<12, xfer_size);
		if (!tsk->dst1)
			return -ENOMEM;
		if (tflags & TEST_FLAGS_PREF)
			memset(tsk->dst1, 0, xfer_size);
	}

	/* allocate memory: seed addr */
	switch (opcode) {
	case DSA_OPCODE_CRCGEN:
		tsk->crc_seed = 0;
		break;

	case DSA_OPCODE_COPY_CRC:
		tsk->seed_addr = malloc(xfer_size);
		if (!tsk->seed_addr)
			return -ENOMEM;
		memset_pattern(tsk->seed_addr, tsk->pattern, xfer_size);
		tsk->crc_seed = 0;
	}

	dbg("Mem allocated: s1 %#lx s2 %#lx d1 %#lx d2 %#lx sd %#lx\n",
			tsk->src1, tsk->src2, tsk->dst1, tsk->dst2, tsk->seed_addr);

	return DSA_STATUS_OK;
}

int dsa_enqcmd(struct dsa_context *ctx, struct dsa_hw_desc *hw)
{
	int retry_count = 0;
	int ret = 0;

	while (retry_count < 3) {
		if (!enqcmd(ctx->wq_reg, hw))
			break;

		info("retry\n");
		retry_count++;
	}

	return ret;
}

static inline unsigned long rdtsc(void)
{
	uint32_t a, d;

	asm volatile("rdtsc" : "=a"(a), "=d"(d));
	return ((uint64_t)d << 32) | (uint64_t)a;
}

static inline void umonitor(volatile void *addr)
{
	asm volatile(".byte 0xf3, 0x48, 0x0f, 0xae, 0xf0" : : "a"(addr));
}

static inline int umwait(unsigned long timeout, unsigned int state)
{
	uint8_t r;
	uint32_t timeout_low = (uint32_t)timeout;
	uint32_t timeout_high = (uint32_t)(timeout >> 32);

	timeout_low = (uint32_t)timeout;
	timeout_high = (uint32_t)(timeout >> 32);

	asm volatile(".byte 0xf2, 0x48, 0x0f, 0xae, 0xf1\t\n"
		"setc %0\t\n"
		: "=r"(r)
		: "c"(state), "a"(timeout_low), "d"(timeout_high));
	return r;
}

static int dsa_wait_on_desc_timeout(struct dsa_completion_record *comp,
		unsigned int msec_timeout)
{
	unsigned int j = 0;

	if (!umwait_support) {
		while (j < msec_timeout && comp->status == 0) {
			usleep(1000);
			j++;
		}
	} else {
		unsigned long timeout = (ms_timeout * 1000000) * 3;
		int r = 1;
		unsigned long t = 0;

		timeout += rdtsc();
		while (comp->status == 0) {
			if (!r) {
				t = rdtsc();
				if (t >= timeout) {
					err("umwait timeout %#lx\n", t);
					break;
				}
			}

			umonitor((uint8_t *)comp);
			if (comp->status != 0)
				break;
			r = umwait(timeout, 0);
		}
		if (t >= timeout)
			j = msec_timeout;
	}

	return (j == msec_timeout) ? -EAGAIN : 0;
}

/* the pattern is 8 bytes long while the dst can with any length */
void memset_pattern(void *dst, uint64_t pattern, size_t len)
{
	size_t len_8_aligned, len_remainding, mask = 0x7;
	uint64_t *aligned_end, *tmp_64;

	/* 8 bytes aligned part */
	len_8_aligned = len & ~mask;
	aligned_end = (uint64_t *)((uint8_t *)dst + len_8_aligned);
	tmp_64 = (uint64_t *)dst;
	while (tmp_64 < aligned_end) {
		*tmp_64 = pattern;
		tmp_64++;
	}

	/* non-aligned part */
	len_remainding = len & mask;
	memcpy(aligned_end, &pattern, len_remainding);
}

/* return 0 if src is a repeatation of pattern, -1 otherwise */
/* the pattern is 8 bytes long and the src could be with any length */
int memcmp_pattern(const void *src, const uint64_t pattern, size_t len)
{
	size_t len_8_aligned, len_remainding, mask = 0x7;
	uint64_t *aligned_end, *tmp_64;

	/* 8 bytes aligned part */
	len_8_aligned = len & ~mask;
	aligned_end = (uint64_t *)((uint8_t *)src + len_8_aligned);
	tmp_64 = (uint64_t *)src;
	while (tmp_64 < aligned_end) {
		if (*tmp_64 != pattern)
			return -1;
		tmp_64++;
	}

	/* non-aligned part */
	len_remainding = len & mask;
	if (memcmp(aligned_end, &pattern, len_remainding))
		return -1;

	return 0;
}

void dsa_free(struct dsa_context *ctx)
{
	if (munmap(ctx->wq_reg, 0x1000))
		err("munmap failed %d\n", errno);

	close(ctx->fd);

	accfg_unref(ctx->ctx);
	dsa_free_task(ctx);
	free(ctx);
}

void dsa_free_task(struct dsa_context *ctx)
{
	free_task(ctx->single_task);
}

void free_task(struct task *tsk)
{
	clean_task(tsk);
	free(tsk->desc);
	free(tsk->comp);
	free(tsk);
}

/* The components of task is free but not the struct task itself */
void clean_task(struct task *tsk)
{
	if (!tsk)
		return;

	free(tsk->src2);
	free(tsk->dst1);
	free(tsk->dst2);
}

int dsa_wait_crcgen(struct dsa_context *ctx)
{
	struct dsa_hw_desc *desc = ctx->single_task->desc;
	struct dsa_completion_record *comp = ctx->single_task->comp;
	int rc;

again:
	rc = dsa_wait_on_desc_timeout(comp, ms_timeout);

	if (rc < 0) {
		err("crcgen desc timeout\n");
		return DSA_STATUS_TIMEOUT;
	}

	/* re-submit if PAGE_FAULT reported by HW && BOF is off */
	if (stat_val(comp->status) == DSA_COMP_PAGE_FAULT_NOBOF &&
			!(desc->flags & IDXD_OP_FLAG_BOF)) {
		dsa_reprep_crcgen(ctx);
		goto again;
	}

	return DSA_STATUS_OK;
}

int dsa_crcgen(struct dsa_context *ctx)
{
	struct task *tsk = ctx->single_task;
	int ret = DSA_STATUS_OK;

	tsk->dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((tsk->test_flags & TEST_FLAGS_BOF) && ctx->bof)
		tsk->dflags |= IDXD_OP_FLAG_BOF;

	dsa_prep_crcgen(tsk);
	dsa_desc_submit(ctx, tsk->desc);
	ret = dsa_wait_crcgen(ctx);

	return ret;
}

int dsa_wait_copycrc(struct dsa_context *ctx)
{
	struct dsa_hw_desc *desc = ctx->single_task->desc;
	struct dsa_completion_record *comp = ctx->single_task->comp;
	int rc;

again:
	rc = dsa_wait_on_desc_timeout(comp, ms_timeout);

	if (rc < 0) {
		err("copy crc desc timeout\n");
		return DSA_STATUS_TIMEOUT;
	}

	/* re-submit if PAGE_FAULT reported by HW && BOF is off */
	if (stat_val(comp->status) == DSA_COMP_PAGE_FAULT_NOBOF &&
			!(desc->flags & IDXD_OP_FLAG_BOF)) {
		dsa_reprep_copycrc(ctx);
		goto again;
	}

	return DSA_STATUS_OK;
}

int dsa_copycrc(struct dsa_context *ctx)
{
	struct task *tsk = ctx->single_task;
	int ret = DSA_STATUS_OK;

	tsk->dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((tsk->test_flags & TEST_FLAGS_BOF) && ctx->bof)
		tsk->dflags |= IDXD_OP_FLAG_BOF;

	dsa_prep_copycrc(tsk);
	dsa_desc_submit(ctx, tsk->desc);
	ret = dsa_wait_copycrc(ctx);

	return ret;
}

/* mismatch_expected: expect mismatched buffer with success status 0x1 */
int task_result_verify(struct task *tsk, int mismatch_expected)
{
	int rc;

	//info("verifying task result for %#lx\n", tsk);

	if (tsk->comp->status != DSA_COMP_SUCCESS)
		return tsk->comp->status;

	switch (tsk->opcode) {
	case DSA_OPCODE_CRCGEN:
		rc = task_result_verify_crcgen(tsk, mismatch_expected);
		return rc;
	case DSA_OPCODE_COPY_CRC:
		rc = task_result_verify_copycrc(tsk, mismatch_expected);
		return rc;
	}

	info("test with op %d passed\n", tsk->opcode);

	return DSA_STATUS_OK;
}

int task_result_verify_crcgen(struct task *tsk, int mismatch_expected)
{
	if (mismatch_expected)
		warn("invalid arg mismatch_expected for %d\n", tsk->opcode);

	/* crc completed */
	if (tsk->comp->status) {
		//info("crc_val=%#x\n", tsk->comp->crc_val);
		return DSA_STATUS_OK;
	}

	err("DSA wrongly cal the buffer\n");
	return -ENXIO;
}

int task_result_verify_copycrc(struct task *tsk, int mismatch_expected)
{
	if (mismatch_expected)
		warn("invalid arg mismatch_expected for %d\n", tsk->opcode);

	/* crc completed */
	if (tsk->comp->status) {
		info("crc_val=%#x\n", tsk->comp->crc_val);
		return DSA_STATUS_OK;
	}

	err("DSA wrongly cal the buffer\n");
	return -ENXIO;
}

void dsa_prep_desc_common(struct dsa_hw_desc *hw, char opcode,
		uint64_t dest, uint64_t src, size_t len, unsigned long dflags)
{
	hw->flags = dflags;
	hw->opcode = opcode;
	hw->src_addr = src;
	hw->dst_addr = dest;
	hw->xfer_size = len;
}

void dsa_desc_submit(struct dsa_context *ctx, struct dsa_hw_desc *hw)
{
	dump_desc(hw);

	/* use MOVDIR64B for DWQ */
	if (ctx->dedicated)
		movdir64b(ctx->wq_reg, hw);
	else /* use ENQCMD for SWQ */
		if (dsa_enqcmd(ctx, hw))
			usleep(10000);
}

void dsa_prep_crcgen(struct task *tsk)
{
	struct dsa_hw_desc *hw = tsk->desc;
	//info("preparing descriptor for crcgen\n");

	dsa_prep_desc_common(tsk->desc, tsk->opcode, (uint64_t)(tsk->dst1),
			(uint64_t)(tsk->src1), tsk->xfer_size, tsk->dflags);
	hw->crc_seed = tsk->crc_seed;
	tsk->desc->completion_addr = (uint64_t)(tsk->comp);
	tsk->comp->status = 0;
}

void dsa_prep_copycrc(struct task *tsk)
{
	struct dsa_hw_desc *hw = tsk->desc;
	info("preparing descriptor for copycrc\n");

	dsa_prep_desc_common(tsk->desc, tsk->opcode, (uint64_t)(tsk->dst1),
			(uint64_t)(tsk->src1), tsk->xfer_size, tsk->dflags);
	hw->seed_addr = (uint64_t)(tsk->seed_addr);
	hw->crc_seed = tsk->crc_seed;
	tsk->desc->completion_addr = (uint64_t)(tsk->comp);
	tsk->comp->status = 0;
}

void dsa_reprep_crcgen(struct dsa_context *ctx)
{
	struct task *tsk = ctx->single_task;
	struct dsa_completion_record *compr = tsk->comp;
	struct dsa_hw_desc *hw = ctx->single_task->desc;

	info("PF addr %#lx dir %d bc %#x\n",
			compr->fault_addr, compr->result,
			compr->bytes_completed);

	hw->xfer_size -= compr->bytes_completed;

	if (compr->result == 0) {
		hw->src_addr += compr->bytes_completed;
	}

	resolve_page_fault(compr->fault_addr, compr->status);

	compr->status = 0;

	dsa_desc_submit(ctx, hw);
}

void dsa_reprep_copycrc(struct dsa_context *ctx)
{
	struct dsa_completion_record *compr = ctx->single_task->comp;
	struct dsa_hw_desc *hw = ctx->single_task->desc;

	info("PF addr %#lx dir %d bc %#x\n",
			compr->fault_addr, compr->result,
			compr->bytes_completed);

	hw->xfer_size -= compr->bytes_completed;

	if (compr->result == 0) {
		hw->src_addr += compr->bytes_completed;
		hw->dst_addr += compr->bytes_completed;
	}

	resolve_page_fault(compr->fault_addr, compr->status);

	compr->status = 0;

	dsa_desc_submit(ctx, hw);
}
