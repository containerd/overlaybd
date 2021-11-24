/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2019 Intel Corporation. All rights reserved. */
#ifndef __DSA_H__
#define __DSA_H__
#include "libaccel_config.h"
#include "idxd.h"

#define MAX_PATH_LENGTH 1024

#define DSA_MAX_OPS 0x20

#define TEST_FLAGS_BOF     0x1     /* Block on page faults */
#define TEST_FLAGS_WAIT    0x4     /* Wait in kernel */
#define TEST_FLAGS_PREF    0x8     /* Pre-fault the buffers */

#define DSA_STATUS_OK    0x0
#define DSA_STATUS_RETRY 0x1
#define DSA_STATUS_FAIL  0x2
#define DSA_STATUS_RPF   0x3
#define DSA_STATUS_URPF  0x4
#define DSA_STATUS_TIMEOUT 0x5

#define DSA_CAP_BLOCK_ON_FAULT                  0x0000000000000001
#define DSA_CAP_OVERLAP_COPY                    0x0000000000000002
#define DSA_CAP_CACHE_MEM_CTRL                  0x0000000000000004
#define DSA_CAP_CACHE_FLUSH_CTRL                0x0000000000000008
#define DSA_CAP_DEST_RDBACK                     0x0000000000000100
#define DSA_CAP_DUR_WRITE                       0x0000000000000200

#define DSA_CAP_MAX_XFER_MASK                   0x00000000001F0000
#define DSA_CAP_MAX_XFER_SHIFT                  16

#define DSA_COMP_STAT_CODE_MASK                 0x3F
#define DSA_COMP_STAT_RW_MASK                   0x80
#define SHARED 1

/* helper macro to get lower 6 bits (ret code) from completion status */
#define stat_val(status) ((status) & DSA_COMP_STAT_CODE_MASK)

extern unsigned int ms_timeout;
extern int debug_logging;

/*
 * The status field will be modified by hardware, therefore it should be
 * __volatile__ and prevent the compiler from optimize the read.
 */
struct dsa_completion_record {
	__volatile__ uint8_t	status;
	union {
		uint8_t		result;
		uint8_t		dif_status;
	};
	uint16_t		rsvd;
	uint32_t		bytes_completed;
	uint64_t		fault_addr;
	union {
		/* common record */
		struct {
			uint32_t	invalid_flags:24;
			uint32_t	rsvd2:8;
		};

		uint32_t	delta_rec_size;
		uint32_t	crc_val;

		/* DIF check & strip */
		struct {
			uint32_t	dif_chk_ref_tag;
			uint16_t	dif_chk_app_tag_mask;
			uint16_t	dif_chk_app_tag;
		};

		/* DIF insert */
		struct {
			uint64_t	dif_ins_res;
			uint32_t	dif_ins_ref_tag;
			uint16_t	dif_ins_app_tag_mask;
			uint16_t	dif_ins_app_tag;
		};

		/* DIF update */
		struct {
			uint32_t	dif_upd_src_ref_tag;
			uint16_t	dif_upd_src_app_tag_mask;
			uint16_t	dif_upd_src_app_tag;
			uint32_t	dif_upd_dest_ref_tag;
			uint16_t	dif_upd_dest_app_tag_mask;
			uint16_t	dif_upd_dest_app_tag;
		};

		uint8_t		op_specific[16];
	};
} __attribute__((packed));

/* metadata for single DSA task */
struct task {
	struct dsa_hw_desc *desc;
	struct dsa_completion_record *comp;
	uint32_t opcode;
	void *src1;
	void *src2;
	void *dst1;
	void *dst2;
	void *seed_addr;
	uint32_t crc_seed;
	uint64_t pattern;
	uint64_t xfer_size;
	uint32_t dflags;
	int test_flags;
};

struct dsa_context {
	struct accfg_ctx *ctx;
	struct accfg_wq *wq;

	unsigned int max_batch_size;
	unsigned int max_xfer_size;
	unsigned int max_xfer_bits;

	int fd;
	int wq_idx;
	void *wq_reg;
	int wq_size;
	int dedicated;
	int bof;
	unsigned int wq_max_batch_size;
	unsigned long wq_max_xfer_size;
	int ats_disable;

	struct task *single_task;
};

static inline void vprint_log(const char *tag, const char *msg, va_list args)
{
	printf("[%5s] ", tag);
	vprintf(msg, args);
}

static inline void vprint_err(const char *tag, const char *msg, va_list args)
{
	fprintf(stderr, "[%5s] ", tag);
	vfprintf(stderr, msg, args);
}

static inline void err(const char *msg, ...)
{
	va_list args;

	va_start(args, msg);
	vprint_err("error", msg, args);
	va_end(args);
}

static inline void warn(const char *msg, ...)
{
	va_list args;

	va_start(args, msg);
	vprint_err("warn", msg, args);
	va_end(args);
}

static inline void info(const char *msg, ...)
{
	va_list args;

	va_start(args, msg);
	vprint_log("info", msg, args);
	va_end(args);
}

static inline void dbg(const char *msg, ...)
{
	va_list args;

	if (!debug_logging)
		return;

	va_start(args, msg);
	vprint_log("debug", msg, args);
	va_end(args);
}

/* Dump DSA hardware descriptor to log */
static inline void dump_desc(struct dsa_hw_desc *hw)
{
	struct dsa_raw_desc *rhw = (struct dsa_raw_desc *)hw;
	int i;

	dbg("desc addr: %p\n", hw);

	for (i = 0; i < 8; i++)
		dbg("desc[%d]: 0x%016lx\n", i, rhw->field[i]);
}

static inline void resolve_page_fault(uint64_t addr, uint8_t status)
{
	uint8_t *addr_u8 = (uint8_t *)addr;

	/* This line solve the PF by writing to the address.*/
	/* For PF at write, we can change the value as the address will be */
	/* overwritten again by the DSA HW */
	*addr_u8 =  ~(*addr_u8);

	/* For PF at read, we need to restore it to the orginal value */
	if (!(status & DSA_COMP_STAT_RW_MASK))
		*addr_u8 = ~(*addr_u8);
}

void memset_pattern(void *dst, uint64_t pattern, size_t len);
int memcmp_pattern(const void *src, const uint64_t pattern, size_t len);
int dsa_enqcmd(struct dsa_context *ctx, struct dsa_hw_desc *hw);

struct dsa_context *dsa_init(void);
int dsa_alloc(struct dsa_context *ctx, int shared);
int alloc_task(struct dsa_context *ctx);
struct task *__alloc_task(void);
int init_task(struct task *tsk, int tflags, int opcode,
		const uint8_t *data, unsigned long xfer_size, uint32_t crc_seed);

int dsa_crcgen(struct dsa_context *ctx);
int dsa_copycrc(struct dsa_context *ctx);
int dsa_wait_crcgen(struct dsa_context *ctx);
int dsa_wait_copycrc(struct dsa_context *ctx);


void dsa_prep_crcgen(struct task *tsk);
void dsa_reprep_crcgen(struct dsa_context *ctx);
void dsa_prep_copycrc(struct task *tsk);
void dsa_reprep_copycrc(struct dsa_context *ctx);

int task_result_verify(struct task *tsk, int mismatch_expected);
int task_result_verify_crcgen(struct task *tsk, int mismatch_expected);
int task_result_verify_copycrc(struct task *tsk, int mismatch_expected);

void dsa_free(struct dsa_context *ctx);
void dsa_free_task(struct dsa_context *ctx);
void free_task(struct task *tsk);
void clean_task(struct task *tsk);

void dsa_prep_desc_common(struct dsa_hw_desc *hw, char opcode,
		uint64_t dest, uint64_t src, size_t len, unsigned long dflags);
void dsa_desc_submit(struct dsa_context *ctx, struct dsa_hw_desc *hw);

static inline void movdir64b(volatile void *portal, void *desc)
{
	asm volatile("sfence\t\n"
			".byte 0x66, 0x0f, 0x38, 0xf8, 0x02\t\n"  :
			: "a" (portal), "d" (desc));
}

static inline unsigned char enqcmd(volatile void *portal, void *desc)
{
	unsigned char retry;
	asm volatile("sfence\t\n"
			".byte 0xf2, 0x0f, 0x38, 0xf8, 0x02\t\n"
			"setz %0\t\n"
			: "=r"(retry): "a" (portal), "d" (desc));
	return retry;
}
#endif
