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
#include "fd-events.h"
#include <errno.h>
#include <unistd.h>
#include <vector>
#include <mutex>
#include <thread>
#include <chrono>
#include <utility>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <boost/lockfree/spsc_queue.hpp>
#include <atomic>
#include <bitset>
#include <sched.h>
#include "../thread.h"
#include "../../utility.h"
#include "../../alog.h"

namespace photon {
#ifndef EPOLLRDHUP
#define EPOLLRDHUP 0
#endif

// maps interface event(s) to epoll defined events
static EventsMap_<EPOLLIN | EPOLLRDHUP, EPOLLOUT> evmap(EVENT_READ, EVENT_WRITE);

static const int EOK = ENXIO;

class EPoll {
public:
    int epfd = -1;
    int init() {
        if (epfd >= 0)
            LOG_ERROR_RETURN(EALREADY, -1, "EPoll already inited");

        epfd = epoll_create(1);
        if (epfd < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to epoll_create(1)");

        return 0;
    }
    int fini() {
        if_close_fd(epfd);
        return 0;
    }
    void if_close_fd(int &fd) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
    ~EPoll() {
        fini();
    }
    int ctl(int fd, int op, uint32_t events, void *data, int no_log_errno_1 = 0,
            int no_log_errno_2 = 0) {
        struct epoll_event ev;
        ev.events = events; // EPOLLERR | EPOLLHUP always included
        ev.data.ptr = data;
        int ret = epoll_ctl(epfd, op, fd, &ev);
        if (ret < 0) {
            ERRNO err;
            if (err.no != no_log_errno_1 &&
                err.no != no_log_errno_2) { // deleting a non-existing fd is considered OK
                auto events = ev.events;
                auto ptr = ev.data.ptr;
                LOG_WARN("failed to call epoll_ctl(`, `, `, `, `)", VALUE(epfd), VALUE(op),
                         VALUE(fd), VALUE(events), VALUE(ptr), err);
            }
            return -err.no;
        }
        return 0;
    }
    void cancel(int fd) {
    again:
        int ret = ctl(fd, EPOLL_CTL_DEL, 0, nullptr, ENOENT, EBADF);
        if (ret < 0 && ret != -ENOENT && ret != -EBADF) {
            thread_usleep(1000);
            goto again;
        }
    }
    int do_wait_for_events(uint64_t timeout) {
        int ret = photon::thread_usleep(timeout);
        auto eno = &errno;
        if (ret == 0) {
            LOG_DEBUG("timeout when wait for events");
            *eno = ETIMEDOUT;
            return -1;
        }
        if (*eno != EOK) {
            LOG_DEBUG("failed when wait for events: ", ERRNO(*eno));
            return -1;
        }
        return 0;
    }
    int wait_for_events(int fd, uint32_t events, uint64_t timeout) {
        int ret = ctl(fd, EPOLL_CTL_ADD, events, CURRENT);
        if (ret < 0)
            return -1;

        DEFER(cancel(fd));
        ret = do_wait_for_events(timeout);
        if (ret < 0)
            LOG_DEBUG(VALUE(fd), VALUE(events));
        return ret;
    }
    int fd_interest(int fd, uint32_t events, void *data) {
        if (events == 0)
            return ctl(fd, EPOLL_CTL_DEL, events, data, ENOENT, EBADF);

        int ret = ctl(fd, EPOLL_CTL_ADD, events, data, EEXIST);
        if (ret == -EEXIST)
            ret = ctl(fd, EPOLL_CTL_MOD, events, data);
        return ret;
    }
    int wait(epoll_event *events, int maxevents, int timeout_ms) {
    again:
        int ret = epoll_wait(epfd, events, maxevents, timeout_ms);
        if (ret == 0)
            return 0; // 0 events occured (timeout)
        if (ret < 0) {
            // NO photon::thread_usleep !!!
            usleep(1000 * 10); // sleep 10ms whenever error occured
            if (errno == EINTR)
                goto again;
            LOG_ERRNO_RETURN(0, -1, "epoll_wait() returned ", ret);
        }
        return ret;
    }
};

template <int MAX_EVENTS>
struct EPWaiter {
    epoll_event events[MAX_EVENTS];
    int n;
    EPWaiter(EPoll *epoll, int timeout_ms) {
        n = epoll->wait(events, MAX_EVENTS, timeout_ms);
    }
    void *operator[](int i) {
        assert(0 <= i && i < MAX_EVENTS);
        return events[i].data.ptr;
    }
};

class MasterEPoll : public EPoll {
public:
    std::atomic<bool> sleeping alignas(64);
    int evfd = -1;
    int init() {
        int ret = EPoll::init();
        if (ret < 0)
            return ret;

        evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (evfd < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to create eventfd");

        ret = ctl(evfd, EPOLL_CTL_ADD, EPOLLIN | EPOLLRDHUP, (void *)-1);
        if (ret < 0) {
            DEFER(close(evfd));
            LOG_ERRNO_RETURN(0, -1, "failed to add eventfd(`) to epollfd(`) ", evfd, epfd);
        }

        sleeping.store(true, std::memory_order_release);
        return 0;
    }
    int fini() {
        EPoll::fini();
        if_close_fd(evfd);
        return 0;
    }
    struct InFlightEvent {
        thread *reader;
        thread *writer;
    };
    std::vector<InFlightEvent> inflight_events;
    // target == 0 for read, 1 for write
    int wait_for_event(uint64_t fd, int event, uint64_t timeout) {
        if (fd >= inflight_events.size())
            inflight_events.resize(fd * 2);
        auto target = event;
        auto actor = &inflight_events[fd].reader;
        if (actor[target])
            LOG_ERROR_RETURN(EALREADY, -1, "already waiting for fd ", fd);
        actor[target] = CURRENT;
        DEFER({ actor[target] = nullptr; });
        static const uint32_t events[] = {evmap.UNDERLAY_EVENT_READ, evmap.UNDERLAY_EVENT_WRITE};

        int ret;
        auto other = target ^ 1;
        if (actor[other]) {
            ret = ctl(fd, EPOLL_CTL_MOD, events[target] | events[other], (void *)fd);
        } else {
            ret = ctl(fd, EPOLL_CTL_ADD, events[target], (void *)fd);
        }
        if (ret < 0)
            return -1;

        ret = do_wait_for_events(timeout);
        // inflight_events might be resized in other threads
        // may need to get new pointer
        actor = &inflight_events[fd].reader;
        if (ret < 0)
            LOG_DEBUG("do_wait_for_events() failed ", VALUE(fd), ERRNO());

        if (actor[other]) {
            ret = ctl(fd, EPOLL_CTL_MOD, events[other], (void *)fd);
            if (ret < 0) {
                cancel(fd);
                return -1;
            }
        } else {
            cancel(fd);
        }
        return ret;
    }
    int wait_for_fd_readable(int fd, uint64_t timeout) {
        return wait_for_event((uint32_t)fd, 0, timeout);
    }
    int wait_for_fd_writable(int fd, uint64_t timeout) {
        return wait_for_event((uint32_t)fd, 1, timeout);
    }
    int wait_and_issue_events(int timeout_ms) {
        EPWaiter<16> result(this, timeout_ms);
        constexpr auto READBIT = evmap.UNDERLAY_EVENT_READ;
        constexpr auto WRITEBIT = evmap.UNDERLAY_EVENT_WRITE;
        constexpr auto ERRBIT = EPOLLERR | EPOLLHUP;
        for (int i = 0; i < result.n; ++i) {
            auto fd = (int64_t)result[i];
            if (fd >= 0) {
                assert((size_t)fd < inflight_events.size());
                if ((result.events[i].events & (READBIT | ERRBIT)) && inflight_events[fd].reader)
                    thread_interrupt(inflight_events[fd].reader, EOK);
                if ((result.events[i].events & (WRITEBIT | ERRBIT)) && inflight_events[fd].writer)
                    thread_interrupt(inflight_events[fd].writer, EOK);
            } else
                do_safe_thread_interrupt();
        }
        return 0;
    }

    std::mutex resumeq_mutex;
    static const uint32_t RQ_MAX = 65536;
    boost::lockfree::spsc_queue<std::pair<thread *, int>, boost::lockfree::capacity<RQ_MAX>>
        resumeq;
    void safe_thread_interrupt(thread *th, int error_number, int mode) {
        if (mode == 1) {
            if (photon::thread_stat(th) != photon::WAITING)
                return;
        } else
            assert(mode == 0);

        {
            // TODO: Replace with spin lock
            std::lock_guard<std::mutex> lock(resumeq_mutex);
            while (!resumeq.push({th, error_number}))
                sched_yield();
        }

        asm volatile("mfence" ::: "memory");
        if (sleeping.load(std::memory_order_acquire)) {
            sleeping.store(false, std::memory_order_release);
            uint64_t x = 1;
            ssize_t ret = ::write(evfd, &x, sizeof(x));
            if (ret != sizeof(x)) {
                ERRNO err;
                sleeping.store(true, std::memory_order_release);
                LOG_ERROR("write evfd ` failed, ret `, err `", evfd, ret, err);
            }
        }
    }
    void do_safe_thread_interrupt() {
        uint64_t x;
        ::read(evfd, &x, sizeof(x));
        sleeping.store(true, std::memory_order_release);
        _unused(x);
        asm volatile("mfence" ::: "memory");
        do {
            std::pair<thread *, int> pops[1024];
            int n = resumeq.pop(pops, 1024);
            for (int i = 0; i < n; ++i) {
                photon::thread_interrupt(pops[i].first, pops[i].second);
            }
        } while (resumeq.read_available() > 0);
    }
};

static MasterEPoll master_epoll;

int wait_for_fd_readable(int fd, uint64_t timeout) {
    return master_epoll.wait_for_fd_readable(fd, timeout);
}

int wait_for_fd_writable(int fd, uint64_t timeout) {
    return master_epoll.wait_for_fd_writable(fd, timeout);
}

int wait_for_fd(FD_Events fd_events, uint64_t timeout) {
    return master_epoll.wait_for_events(fd_events.fd, evmap.translate_bitwisely(fd_events.events),
                                        timeout);
}

static int wait_and_issue_events(uint64_t timeout) {
    uint64_t timeout_ms = timeout / 1000;
    if (timeout_ms > INT32_MAX)
        timeout_ms = -1;
    return master_epoll.wait_and_issue_events((int32_t)timeout_ms);
}

int fd_events_epoll_init() {
    LOG_INFO("init event engine: epoll");
    set_idle_sleeper(&wait_and_issue_events);
    return master_epoll.init();
}

int fd_events_epoll_fini() {
    LOG_INFO("finit event engine: epoll");
    set_idle_sleeper(nullptr);
    return master_epoll.fini();
}

void safe_thread_interrupt(thread *th, int error_number, int mode) {
    return master_epoll.safe_thread_interrupt(th, error_number, mode);
}

FD_Poller *new_fd_poller(void *) {
    auto poller = new EPoll;
    if (!poller)
        LOG_ERRNO_RETURN(0, nullptr, "failed to create EPoll()");

    int ret = poller->init();
    if (ret < 0) {
        delete poller;
        LOG_ERROR_RETURN(0, nullptr, "failed to EPoll.init()");
    }

    return (FD_Poller *)poller;
}

int delete_fd_poller(FD_Poller *poller_) {
    auto poller = (EPoll *)poller_;
    delete poller;
    return 0;
}

// removing the fd is to show no interest (0) on the fd
int fd_interest(FD_Poller *poller_, FD_Events fd_events, void *data) {
    auto poller = (EPoll *)poller_;
    return poller->fd_interest(fd_events.fd, evmap.translate_bitwisely(fd_events.events), data);
}

int wait_for_fds(FD_Poller *poller_, void **data, int count, uint64_t timeout) {
    if (!poller_ || !data || count <= 0)
        LOG_ERROR_RETURN(-1, EINVAL, "invalid argument(s)");

    auto poller = (EPoll *)poller_;
    int ret = wait_for_fd_readable(poller->epfd, timeout);
    if (ret < 0) {
        ERRNO eno;
        if (eno.no == ETIMEDOUT || eno.no == EINTR)
            // timeout may not need put log
            // so do interrupt by other thread
            return -1;
        LOG_ERRNO_RETURN(0, -1, "failed to wait for epoll fd ", poller->epfd);
    }

    EPWaiter<16> result(poller, 0);
    assert(result.n != 0);
    if (result.n > count)
        result.n = count;
    for (int i = 0; i < result.n; ++i)
        data[i] = result[i];
    return result.n;
}

} // namespace photon
