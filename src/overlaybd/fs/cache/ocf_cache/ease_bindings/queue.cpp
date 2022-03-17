#include "queue.h"

#include "../../../../photon/thread-pool.h"

static void *run(void *args) {
    auto queue = (ocf_queue_t)args;
    ocf_queue_run(queue);
    return nullptr;
}

/* Pooled queue kicker */
class QueueKicker {
public:
    explicit QueueKicker(ocf_queue_t queue) : m_queue(queue) {
    }

    inline void kick() {
        m_pool.thread_create(run, m_queue);
    }

private:
    /* associated OCF queue */
    ocf_queue_t m_queue;
    /* thread pool */
    photon::ThreadPool<64> m_pool;
};

int init_queues(ocf_queue_t mngt_queue, ocf_queue_t io_queue) {
    auto mngt_queue_kicker = new QueueKicker(mngt_queue);
    auto io_queue_kicker = new QueueKicker(io_queue);

    ocf_queue_set_priv(mngt_queue, mngt_queue_kicker);
    ocf_queue_set_priv(io_queue, io_queue_kicker);
    return 0;
}

/* Callback for OCF to kick the queue thread */
static void queue_thread_kick(ocf_queue_t q) {
    auto qk = (QueueKicker *)ocf_queue_get_priv(q);
    qk->kick();
}

/* Callback for OCF to stop the queue thread */
static void queue_thread_stop(ocf_queue_t q) {
    auto qk = (QueueKicker *)ocf_queue_get_priv(q);
    delete qk;
}

/* Queue ops */
static const ocf_queue_ops queue_ops = {
    .kick = queue_thread_kick,
    .kick_sync = nullptr,
    .stop = queue_thread_stop,
};

const ocf_queue_ops *get_queue_ops() {
    return &queue_ops;
}
