#include "export.h"
#include "thread.h"

int photon_thread_usleep(uint64_t useconds) {
    return photon::thread_usleep(useconds);
}

int photon_thread_sleep(uint64_t seconds) {
    return photon::thread_sleep(seconds);
}