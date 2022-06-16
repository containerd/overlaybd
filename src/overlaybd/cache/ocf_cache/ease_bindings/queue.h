#pragma once

extern "C" {
#include <ocf/ocf.h>
}

int init_queues(ocf_queue_t mngt_queue, ocf_queue_t io_queue);

const ocf_queue_ops *get_queue_ops();
