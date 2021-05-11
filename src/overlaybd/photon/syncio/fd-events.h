/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#pragma once
#include <assert.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/types.h>

namespace photon {
// init the epoll event engine, installing to photon
// an idle sleeping handler, which watches fd events by epoll_wait()
extern "C" int fd_events_epoll_init();
extern "C" int fd_events_epoll_fini();

// init the select event engine, installing to photon
// an idle sleeping handler, which watches fd events by select()
extern "C" int fd_events_select_init();
extern "C" int fd_events_select_fini();

inline int fd_events_init() {
    return
#if defined(__linux__) && !defined(SELECT)
        fd_events_epoll_init();
#else
        fd_events_select_init();
#endif
}
inline int fd_events_fini() {
    return
#if defined(__linux__) && !defined(SELECT)
        fd_events_epoll_fini();
#else
        fd_events_select_fini();
#endif
}

const static uint32_t EVENT_READ = 1;
const static uint32_t EVENT_WRITE = 2;
struct FD_Events {
    int fd;
    uint32_t events; // bitwised EVENT_READ, EVENT_WRITE
};

// blocks current photon thread, and wait
// for the fd to become readable / writable
extern "C" int wait_for_fd_readable(int fd, uint64_t timeout = -1);
extern "C" int wait_for_fd_writable(int fd, uint64_t timeout = -1);
extern "C" int wait_for_fd(FD_Events fd_events, uint64_t timeout = -1);
inline int wait_for_fd(int fd, uint32_t events, uint64_t timeout = -1) {
    return wait_for_fd({fd, events}, timeout);
}

class FD_Poller;
extern "C" FD_Poller *new_fd_poller(void *args);
extern "C" int delete_fd_poller(FD_Poller *poller);

// set an event interest on the fd, may be adding new FD_Event
// interest, or modify or remove existing FD_Event interest.
extern "C" int fd_interest(FD_Poller *poller, FD_Events fd_events, void *data);

// wait for the fds, returns # of events got, and their associated `data`
extern "C" int wait_for_fds(FD_Poller *poller, void ** /*OUT*/ data, int count,
                            uint64_t timeout = -1);

class FD_Poller {
protected:
    FD_Poller() {
    }
    ~FD_Poller() {
    }

public:
    int fd_interest(FD_Events fd_events, void *data) {
        return photon::fd_interest(this, fd_events, data);
    }
    int fd_no_interest(int fd) {
        return fd_interest({fd, 0}, nullptr);
    }
    // wait for the fds, returns # of events got, and their associated `data`
    ssize_t wait_for_fds(void ** /*OUT*/ data, size_t count, uint64_t timeout = -1) {
        return photon::wait_for_fds(this, data, count, timeout);
    }
};

struct thread;

// a helper class to translate events into underlay representation
template <uint32_t UNDERLAY_EVENT_READ_, uint32_t UNDERLAY_EVENT_WRITE_>
struct EventsMap_ {
    const static uint32_t UNDERLAY_EVENT_READ = UNDERLAY_EVENT_READ_;
    const static uint32_t UNDERLAY_EVENT_WRITE = UNDERLAY_EVENT_WRITE_;
    static_assert(UNDERLAY_EVENT_READ != UNDERLAY_EVENT_WRITE, "...");
    static_assert(UNDERLAY_EVENT_READ, "...");
    static_assert(UNDERLAY_EVENT_WRITE, "...");

    uint64_t ev_read, ev_write;
    EventsMap_(uint64_t event_read, uint64_t event_write) {
        ev_read = event_read;
        ev_write = event_write;
        assert(ev_read);
        assert(ev_write);
        assert(ev_read != ev_write);
    }
    uint32_t translate_bitwisely(uint64_t events) const {
        uint32_t ret = 0;
        if (events & ev_read)
            ret |= UNDERLAY_EVENT_READ;
        if (events & ev_write)
            ret |= UNDERLAY_EVENT_WRITE;
        return ret;
    }
    uint32_t translate_byval(uint64_t event) const {
        if (event == ev_read)
            return UNDERLAY_EVENT_READ;
        if (event == ev_write)
            return UNDERLAY_EVENT_WRITE;
    }
};

typedef EventsMap_<EVENT_READ, EVENT_WRITE> EventsMap;
} // namespace photon
