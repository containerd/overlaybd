/* lz4-qat.cpp — Intel QAT LZ4 batch decompress for ZFile */

#include "lz4-qat.h"
#include "lz4.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <photon/thread/thread.h>

extern "C" {
#include "qat/cpa.h"
#include "qat/cpa_dc.h"
#include "qat/icp_sal_user.h"
#include "qat/icp_sal_poll.h"
#include "qat/qae_mem.h"
}

#define QAT_ALIGN          4096
#define QAT_POLL_TIMEOUT_S 30.0

/* photon::mutex yields the coroutine on contention instead of blocking the OS
 * thread (and all other coroutines on the same vCPU). */
static photon::mutex   g_init_mtx;
static int             g_init_refcnt = 0;
static int             g_num_instances = 0;
static int             g_next_inst_idx = 0;
static CpaInstanceHandle *g_instances = nullptr;
static bool            *g_inst_started = nullptr;

static double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static CpaPhysicalAddr v2p(void *va) {
    return (CpaPhysicalAddr)qaeVirtToPhysNUMA(va);
}

/* ---------- DMA slot ---------- */
struct qat_dma_slot {
    void          *comp_buf;
    void          *decomp_buf;
    void          *meta_src;
    void          *meta_dst;
    CpaFlatBuffer *src_flat;
    CpaFlatBuffer *dst_flat;
    CpaBufferList *src_list;
    CpaBufferList *dst_list;
    CpaDcOpData   *op_data;
    CpaDcRqResults *results;
};

static void *qmem_alloc(size_t sz) {
    void *p = qaeMemAllocNUMA(sz, 0, QAT_ALIGN);
    if (p) memset(p, 0, sz);
    return p;
}
static void qmem_free(void *p) {
    if (p) qaeMemFreeNUMA(&p);
}

static int alloc_slot(qat_dma_slot *s, Cpa32U meta_size) {
    s->comp_buf   = qmem_alloc(ZFILE_QAT_MAX_BLOCK_SIZE * 2);
    s->decomp_buf = qmem_alloc(ZFILE_QAT_MAX_BLOCK_SIZE);
    if (meta_size) {
        s->meta_src = qmem_alloc(meta_size);
        s->meta_dst = qmem_alloc(meta_size);
    }
    s->src_flat = (CpaFlatBuffer*)qmem_alloc(sizeof(CpaFlatBuffer));
    s->dst_flat = (CpaFlatBuffer*)qmem_alloc(sizeof(CpaFlatBuffer));
    s->src_list = (CpaBufferList*)qmem_alloc(sizeof(CpaBufferList));
    s->dst_list = (CpaBufferList*)qmem_alloc(sizeof(CpaBufferList));
    s->op_data  = (CpaDcOpData*)qmem_alloc(sizeof(CpaDcOpData));
    s->results  = (CpaDcRqResults*)qmem_alloc(sizeof(CpaDcRqResults));
    if (!s->comp_buf || !s->decomp_buf ||
        !s->src_flat || !s->dst_flat || !s->src_list || !s->dst_list ||
        !s->op_data  || !s->results ||
        (meta_size && (!s->meta_src || !s->meta_dst))) {
        return -1;
    }
    return 0;
}

static void free_slot(qat_dma_slot *s) {
    qmem_free(s->results);   qmem_free(s->op_data);
    qmem_free(s->dst_list);  qmem_free(s->src_list);
    qmem_free(s->dst_flat);  qmem_free(s->src_flat);
    qmem_free(s->meta_dst);  qmem_free(s->meta_src);
    qmem_free(s->decomp_buf);qmem_free(s->comp_buf);
    *s = {};
}

/* ---------- process-global DMA pool, shared across all LZ4_qat_param ---------- */
struct dma_pool_t {
    qat_dma_slot               slots[ZFILE_QAT_POOL_SIZE];
    Cpa32U                     meta_size;
    photon::mutex              mtx;
    photon::condition_variable avail;
    int                        free_stack[ZFILE_QAT_POOL_SIZE];
    int                        free_count;
    bool                       initialized;
    /* Set on poll/drain timeout. Acquires bail with -1 thereafter; tainted
     * slots from the failed batch are leaked since QAT may still write them. */
    bool                       dead;
};
static dma_pool_t g_pool;

/* Caller must hold g_init_mtx. Picks up meta_size from g_instances[0]. */
static int dma_pool_init_locked() {
    if (g_pool.initialized || g_pool.dead) return g_pool.initialized ? 0 : -1;
    if (!g_num_instances) return -1;
    if (cpaDcBufferListGetMetaSize(g_instances[0], 1, &g_pool.meta_size) != CPA_STATUS_SUCCESS) {
        g_pool.meta_size = 0;
    }
    for (int i = 0; i < ZFILE_QAT_POOL_SIZE; i++) {
        if (alloc_slot(&g_pool.slots[i], g_pool.meta_size) != 0) {
            for (int j = 0; j <= i; j++) free_slot(&g_pool.slots[j]);
            return -1;
        }
        g_pool.free_stack[i] = i;
    }
    g_pool.free_count  = ZFILE_QAT_POOL_SIZE;
    g_pool.initialized = true;
    return 0;
}

/* Caller must hold g_init_mtx. Only safe when g_init_refcnt == 0 (no users). */
static void dma_pool_destroy_locked() {
    if (!g_pool.initialized) return;
    if (g_pool.dead) {
        /* Some slots tainted by in-flight QAT writes; don't free, leak them. */
        return;
    }
    for (int i = 0; i < ZFILE_QAT_POOL_SIZE; i++) free_slot(&g_pool.slots[i]);
    g_pool.initialized = false;
    g_pool.free_count  = 0;
}

/* Returns 0 + n indices in out_idx, or -1 if pool dead. Blocks the calling
 * coroutine (yields vCPU) when fewer than n slots are free. */
static int dma_pool_acquire(int *out_idx, size_t n) {
    photon::scoped_lock _l(g_pool.mtx);
    while (true) {
        if (g_pool.dead) return -1;
        if (g_pool.free_count >= (int)n) {
            for (size_t i = 0; i < n; i++) {
                out_idx[i] = g_pool.free_stack[--g_pool.free_count];
            }
            return 0;
        }
        g_pool.avail.wait(_l);
    }
}

static void dma_pool_release(const int *idx, size_t n) {
    photon::scoped_lock _l(g_pool.mtx);
    for (size_t i = 0; i < n; i++) {
        g_pool.free_stack[g_pool.free_count++] = idx[i];
    }
    g_pool.avail.notify_all();
}

/* Mark pool dead and unblock all waiters. Tainted slots from the caller's
 * batch are NOT released (silently leaked) because QAT may still DMA-write. */
static void dma_pool_kill() {
    photon::scoped_lock _l(g_pool.mtx);
    g_pool.dead = true;
    g_pool.avail.notify_all();
}

/* ---------- SAL lifecycle ---------- */
static int sal_user_start_locked() {
    if (g_init_refcnt > 0) {
        g_init_refcnt++;
        return 0;
    }
    /* Section name varies by /etc/4xxx_dev0.conf; try common ones. */
    if (icp_sal_userStart("SHIM") != CPA_STATUS_SUCCESS) {
        if (icp_sal_userStart("SSL") != CPA_STATUS_SUCCESS) {
            fprintf(stderr, "[lz4-qat] icp_sal_userStart failed (try section SHIM/SSL)\n");
            return -1;
        }
    }
    Cpa16U n = 0;
    if (cpaDcGetNumInstances(&n) != CPA_STATUS_SUCCESS || n == 0) {
        fprintf(stderr, "[lz4-qat] cpaDcGetNumInstances: 0 instances\n");
        icp_sal_userStop();
        return -1;
    }
    g_instances = (CpaInstanceHandle*)calloc(n, sizeof(CpaInstanceHandle));
    if (!g_instances) { icp_sal_userStop(); return -1; }
    if (cpaDcGetInstances(n, g_instances) != CPA_STATUS_SUCCESS) {
        free(g_instances); g_instances = nullptr;
        icp_sal_userStop();
        return -1;
    }
    g_num_instances = n;
    g_next_inst_idx = 0;
    g_inst_started = (bool*)calloc(n, sizeof(bool));
    if (!g_inst_started) { free(g_instances); g_instances = nullptr; icp_sal_userStop(); return -1; }
    g_init_refcnt = 1;
    return 0;
}

static void sal_user_stop_locked() {
    if (--g_init_refcnt > 0) return;
    /* refcnt=0: last user gone, tear down pool too (or leak if dead). */
    dma_pool_destroy_locked();
    if (g_instances) {
        for (int i = 0; i < g_num_instances; i++) {
            if (g_inst_started && g_inst_started[i])
                cpaDcStopInstance(g_instances[i]);
        }
        free(g_inst_started); g_inst_started = nullptr;
        free(g_instances);
        g_instances = nullptr;
    }
    g_num_instances = 0;
    icp_sal_userStop();
}

/* ---------- callback ---------- */
static void qat_dc_cb(void *cb_tag, CpaStatus status) {
    LZ4_qat_param *p = (LZ4_qat_param*)cb_tag;
    if (!p) return;
    if (status != CPA_STATUS_SUCCESS) {
        p->errors.fetch_add(1, std::memory_order_relaxed);
    }
    p->completed.fetch_add(1, std::memory_order_relaxed);
}

/* ---------- public API ---------- */
int qat_init(LZ4_qat_param *p) {
    if (!p) return -1;

    {
        photon::scoped_lock _l(g_init_mtx);
        if (sal_user_start_locked() != 0) return -1;
        if (dma_pool_init_locked() != 0) {
            fprintf(stderr, "[lz4-qat] global DMA pool init failed\n");
            sal_user_stop_locked();
            return -1;
        }
        /* Round-robin so concurrent LZ4_qat_param spread across QAT rings. */
        int idx = (g_next_inst_idx++) % g_num_instances;
        p->inst = g_instances[idx];
    }

    cpaDcSetAddressTranslation(p->inst, v2p);
    /* Find which slot, start the instance if not already started. */
    {
        photon::scoped_lock _l(g_init_mtx);
        int my_idx = -1;
        for (int i = 0; i < g_num_instances; i++) {
            if (g_instances[i] == p->inst) { my_idx = i; break; }
        }
        if (my_idx >= 0 && g_inst_started && !g_inst_started[my_idx]) {
            if (cpaDcStartInstance(p->inst, 0, NULL) != CPA_STATUS_SUCCESS) {
                fprintf(stderr, "[lz4-qat] cpaDcStartInstance failed\n");
                sal_user_stop_locked();
                p->inst = nullptr;
                return -1;
            }
            g_inst_started[my_idx] = true;
        }
    }
    {
        const char *env = getenv("ZFILE_QAT_RATIO_THRESHOLD");
        if (env) {
            double v = atof(env);
            if (v > 0) p->ratio_threshold = v;
        }
    }
    return 0;
}

int qat_uninit(LZ4_qat_param *p) {
    if (!p) return -1;
    /* No per-param DMA to free — pool is shared and torn down on last user. */
    {
        photon::scoped_lock _l(g_init_mtx);
        sal_user_stop_locked();
    }
    p->inst = nullptr;
    return 0;
}

void qat_set_ratio_threshold(LZ4_qat_param *p, double ratio) {
    if (p && ratio > 0) p->ratio_threshold = ratio;
}

/* Returns 0 on QAT success, -1 on any QAT issue. CPU fallback lives in
 * compressor.cpp's existing LZ4_decompress_safe loop; not duplicated here. */
int LZ4_decompress_qat(LZ4_qat_param *p,
                       unsigned char **src, size_t *src_len,
                       unsigned char **dst, size_t *dst_len,
                       size_t n) {
    if (n == 0) return 0;
    if (!p || !p->inst || n > ZFILE_QAT_POOL_SIZE) return -1;

    /* dst_len[i] in = block capacity (zfile uses block_size, typically 4KB).
     * src_len[i] is the COMPRESSED data length WITHOUT the 4-byte header;
     * the header is prepended below before submission. total_compressed is
     * used for ratio calculation only and correctly excludes the header. */
    size_t total_compressed = 0, total_raw = 0;
    for (size_t i = 0; i < n; i++) {
        total_compressed += src_len[i];
        total_raw        += dst_len[i];
        /* Allow room for the 4-byte stateless header we prepend later */
        if (src_len[i] == 0 ||
            src_len[i] + sizeof(Cpa32U) > ZFILE_QAT_MAX_BLOCK_SIZE * 2 ||
            dst_len[i]  > ZFILE_QAT_MAX_BLOCK_SIZE) {
            return -1;
        }
    }
    double ratio = total_compressed > 0
                       ? (double)total_raw / (double)total_compressed
                       : 1.0;
    if (ratio < p->ratio_threshold)
        return -1;

    int slot_idx[ZFILE_QAT_POOL_SIZE];
    if (dma_pool_acquire(slot_idx, n) != 0) return -1;

    /* QAT stateless LZ4 decompression requires a 4-byte little-endian header
     * prepended to the compressed data, containing the compressed block size.
     * Standard LZ4 data (from LZ4_compress_default) lacks this header, so we
     * insert it here. This makes QAT decompression compatible with both
     * QAT-compressed and standard-LZ4-compressed data. */
    for (size_t i = 0; i < n; i++) {
        Cpa32U hdr = (Cpa32U)src_len[i];
        memcpy(g_pool.slots[slot_idx[i]].comp_buf, &hdr, sizeof(hdr));
        memcpy((uint8_t*)g_pool.slots[slot_idx[i]].comp_buf + sizeof(hdr),
               src[i], src_len[i]);
    }

    p->completed.store(0, std::memory_order_relaxed);
    p->errors.store(0, std::memory_order_relaxed);

    CpaDcNsSetupData ns;
    memset(&ns, 0, sizeof(ns));
    ns.compLevel            = CPA_DC_L1;
    ns.compType             = CPA_DC_LZ4;
    ns.sessDirection        = CPA_DC_DIR_DECOMPRESS;
    ns.sessState            = CPA_DC_STATELESS;
    ns.windowSize           = CPA_DC_WINSIZE_32K;
    ns.lz4BlockMaxSize      = CPA_DC_LZ4_MAX_BLOCK_SIZE_64K;
    ns.lz4BlockChecksum     = CPA_FALSE;
    ns.lz4BlockIndependence = CPA_TRUE;
    ns.checksum             = CPA_DC_NONE;

    int submitted = 0;
    for (size_t i = 0; i < n; i++) {
        qat_dma_slot *s = &g_pool.slots[slot_idx[i]];
        /* src_len[i] does NOT include the 4-byte stateless header we prepended,
         * so the actual buffer length sent to QAT is src_len[i] + 4. */
        s->src_flat->dataLenInBytes = (Cpa32U)(src_len[i] + sizeof(Cpa32U));
        s->src_flat->pData          = (Cpa8U*)s->comp_buf;
        s->dst_flat->dataLenInBytes = (Cpa32U)dst_len[i];
        s->dst_flat->pData          = (Cpa8U*)s->decomp_buf;
        s->src_list->numBuffers     = 1;
        s->src_list->pBuffers       = s->src_flat;
        s->src_list->pPrivateMetaData = s->meta_src;
        s->dst_list->numBuffers     = 1;
        s->dst_list->pBuffers       = s->dst_flat;
        s->dst_list->pPrivateMetaData = s->meta_dst;
        memset(s->op_data, 0, sizeof(CpaDcOpData));
        s->op_data->flushFlag = CPA_DC_FLUSH_FINAL;

        CpaStatus st = cpaDcNsDecompressData(p->inst, &ns,
                          s->src_list, s->dst_list,
                          s->op_data, s->results,
                          qat_dc_cb, p);
        if (st != CPA_STATUS_SUCCESS) {
            fprintf(stderr, "[lz4-qat] submit[%zu]=%d, drain & give up\n", i, st);
            /* Drain submitted in-flight ops; if drain itself times out, kill pool. */
            double dl = now_ms() + QAT_POLL_TIMEOUT_S * 1000.0;
            while (p->completed.load() < submitted) {
                icp_sal_DcPollInstance(p->inst, 0);
                photon::thread_yield();
                if (now_ms() > dl) {
                    fprintf(stderr, "[lz4-qat] drain timeout, kill pool\n");
                    dma_pool_kill();
                    /* Return without releasing slots — they may still be DMA targets. */
                    return -1;
                }
            }
            /* Drain succeeded: nothing in flight, slots safe to return. */
            dma_pool_release(slot_idx, n);
            return -1;
        }
        submitted++;
    }

    double deadline = now_ms() + QAT_POLL_TIMEOUT_S * 1000.0;
    while (p->completed.load(std::memory_order_acquire) < (int)n) {
        icp_sal_DcPollInstance(p->inst, 0);
        photon::thread_yield();
        if (now_ms() > deadline) {
            fprintf(stderr, "[lz4-qat] poll timeout completed=%d/%zu, kill pool\n",
                    p->completed.load(), n);
            dma_pool_kill();
            return -1;
        }
    }
    if (p->errors.load() > 0) {
        fprintf(stderr, "[lz4-qat] %d errors in batch n=%zu\n", p->errors.load(), n);
        dma_pool_release(slot_idx, n);
        return -1;
    }

    for (size_t i = 0; i < n; i++) {
        Cpa32U produced = g_pool.slots[slot_idx[i]].results->produced;
        if (produced == 0 || produced > dst_len[i]) {
            fprintf(stderr, "[lz4-qat] block[%zu] bad produced=%u cap=%zu\n",
                    i, produced, dst_len[i]);
            dma_pool_release(slot_idx, n);
            return -1;
        }
    }

    for (size_t i = 0; i < n; i++) {
        Cpa32U produced = g_pool.slots[slot_idx[i]].results->produced;
        memcpy(dst[i], g_pool.slots[slot_idx[i]].decomp_buf, produced);
        dst_len[i] = produced;
    }
    dma_pool_release(slot_idx, n);
    return 0;
}

/* Stub. Always returns -1 so compressor.cpp falls through to LZ4_compress_default. */
int LZ4_compress_qat(LZ4_qat_param *p,
                     unsigned char **src, size_t *src_len,
                     unsigned char **dst, size_t *dst_len,
                     size_t n) {
    (void)p; (void)src; (void)src_len; (void)dst; (void)dst_len; (void)n;
    return -1;
}
