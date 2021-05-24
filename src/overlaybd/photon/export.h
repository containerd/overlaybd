
#pragma once
#include <errno.h>
#include <inttypes.h>

#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

EXTERNC int photon_thread_usleep(uint64_t useconds);
EXTERNC int photon_thread_sleep(uint64_t seconds);
