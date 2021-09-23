#include <stdio.h>
#include "../zfile/crc32/crc32c.h"
#include "../../photon/thread11.h"
#include "../../alog.h"

const int buf_size = 4096;
uint64_t qps = 0;

int worker() {
    while (true) {
        void* buf = malloc(buf_size);
        uint64_t pattern = 0x0123456789abcdef;
        memset_pattern(buf, pattern, buf_size);
        // TODO: fill in random data ?
        int ret = FileSystem::crc32::crc32c(buf, buf_size);
        qps++;
        LOG_INFO("crc ret = `", (uint32_t)ret);
        photon::thread_sleep(1);
        free(buf);
    }
    return 0;
}

void show_qps() {
    while (true) {
        photon::thread_sleep(1);
        LOG_INFO("qps = `", qps);
        qps = 0;
    }
}

int main() {

    photon::thread_create11(show_qps);

    for (int i = 0; i < 32; i++) {
        photon::thread_create11(worker);
    }

    photon::thread_sleep(-1);
}