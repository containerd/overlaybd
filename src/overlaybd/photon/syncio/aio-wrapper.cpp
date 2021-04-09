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
#include "aio-wrapper.h"
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <aio.h>
#include <libaio.h>
#include <signal.h>
#include "../thread.h"
#include "fd-events.h"
#include "../../utility.h"
#include "../../alog.h"

namespace photon {
const int EOK = ENXIO;
const uint64_t IODEPTH = 2048;
static int evfd, running;
static io_context_t aio_ctx;
static thread *polling_thread = nullptr;
static condition_variable cond;

template <typename F>
ssize_t have_n_try(const F &f, const char *name, ssize_t error_level = 0) {
    int ntry = 0;
    while (true) {
        ssize_t ret = f();
        if (ret >= error_level)
            return ret;

        auto e = errno;
        thread_usleep(1000 * 10); // sleep 10ms whenever error occurs
        if (e == EINTR)
            continue;
        if (e == ECANCELED || ntry == 7)
            return ret;
        LOG_WARN("failed to do `() for the `-th time ` ", name, ntry + 1, VALUE(ret), ERRNO(e));
        thread_usleep(1000 * 10 * (1 << ntry++));
    }
}

#define HAVE_N_TRY_(func, args, error_level)                                                       \
    have_n_try([&]() __INLINE__ { return func args; }, #func, error_level)

#define HAVE_N_TRY(func, args) HAVE_N_TRY_(func, args, 0)

struct libaiocb : public iocb {
    ssize_t ioret;
    void cancel() {
        struct io_event cancel_ret;
        HAVE_N_TRY(io_cancel, (aio_ctx, this, &cancel_ret));
    }
    ssize_t submit_and_wait(uint64_t timedout = -1) {
        io_set_eventfd(this, evfd);
        this->data = CURRENT;
        auto piocb = (iocb *)this;

        while (true) {
            int ret = io_submit(aio_ctx, 1, &piocb);
            if (ret == 1)
                break;
            thread_usleep(1000 * 10); // sleep 10ms whenever error occurs
            if (ret < 0) {
                auto e = -ret;
                switch (e) {
                    case EAGAIN:
                        cond.wait_no_lock();
                    case EINTR:
                        continue;
                    case EBADF:
                    case EFAULT:
                    case EINVAL:
                    default:
                        errno = e;
                        LOG_ERRNO_RETURN(0, ret, "failed to io_submit()");
                }
                return -1;
            }
        }

        int ret = thread_usleep(timedout);
        if (ret == 0) // timedout
        {
            cancel();
            LOG_WARN("libaio timedout fd=`, offset=`, nbytes=`", aio_fildes, u.c.offset,
                     u.c.nbytes);
            errno = ETIMEDOUT;
            return -1;
        }

        auto e = errno;
        if (e != EOK) // interrupted by a user thread
        {
            cancel();
            LOG_ERROR_RETURN(e, -1, "libaio interrupted");
        }

        if (this->ioret < 0) {
            e = -this->ioret;
            LOG_ERROR_RETURN(e, -1, "libaio result error");
        }

        return this->ioret;
    }
    template <typename F, typename... ARGS>
    ssize_t asyncio(F io_prep, ARGS... args) {
        io_prep(this, args...);
        return submit_and_wait();
    }
};

static int my_io_getevents(long min_nr, long nr, struct io_event *events) {
    int ret = ::io_getevents(aio_ctx, min_nr, nr, events, NULL);
    if (ret < 0)
        errno = -ret;
    return ret;
}

static void resume_libaio_requesters() {
    struct io_event events[IODEPTH];
    int n = HAVE_N_TRY(my_io_getevents, (0, IODEPTH, events));
    for (int i = 0; i < n; ++i) {
        auto piocb = (libaiocb *)events[i].obj;
        piocb->ioret = events[i].res;
        if (events[i].res2 < 0)
            LOG_WARN("libaio delievers error, ", VALUE(events[i].res), VALUE(events[i].res2),
                     VALUE(events[i].obj), VALUE(piocb->aio_lio_opcode), VALUE(piocb->aio_fildes),
                     VALUE(piocb->u.c.offset), VALUE(piocb->u.c.nbytes), VALUE(piocb->u.c.buf),
                     VALUE(piocb->u.c.resfd));
        thread_interrupt((thread *)events[i].data, EOK);
    }
}

static uint64_t wait_for_events() {
    auto ret = HAVE_N_TRY(wait_for_fd_readable, (evfd));
    if (ret < 0)
        return 0;

    uint64_t nevents = 0;
    HAVE_N_TRY_(::read, (evfd, &nevents, sizeof(nevents)), sizeof(nevents));
    return nevents;
}

static void *libaio_polling(void *) {
    running = 1;
    DEFER(running = 0);
    while (running == 1) {
        running = 2;
        wait_for_events();
        if (running == -1)
            break;
        running = 1;
        resume_libaio_requesters();
        cond.notify_all();
    }
    return nullptr;
}

struct posix_aiocb : public aiocb {
    thread *th;
    ssize_t ioret;
    posix_aiocb(int fd) {
        memset(this, 0, sizeof(aiocb));
        th = CURRENT;
        aio_fildes = fd;
        aio_sigevent.sigev_notify = SIGEV_THREAD;
        aio_sigevent.sigev_notify_function = &aio_completion_handler;
        aio_sigevent.sigev_value.sival_ptr = this;
    }
    static void aio_completion_handler(sigval_t sigval) {
        auto req = (struct posix_aiocb *)sigval.sival_ptr;
        req->ioret = aio_return(req);
        // interrupt current or next sleep in th. note that
        // interrupt may call even before th comming into sleep
        safe_thread_interrupt(req->th, EOK, 0);
    }
    template <typename F, typename... ARGS>
    ssize_t async_perform(F iofunc, ARGS... args) {
        int ret = iofunc(args..., this);
        if (ret < 0)
            return ret;

    again:
        thread_usleep(-1);
        ERRNO e;
        if (e.no != EOK) {
            LOG_ERROR("unexpected wakeup!", e);
            goto again;
        }
        return this->ioret;
    }
    void prep_io(void *buf, size_t count, off_t offset) {
        aio_buf = buf;
        aio_nbytes = count;
        aio_offset = offset;
    }
    ssize_t pread(void *buf, size_t count, off_t offset) {
        prep_io(buf, count, offset);
        return async_perform(&::aio_read);
    }
    ssize_t pwrite(void *buf, size_t count, off_t offset) {
        prep_io(buf, count, offset);
        return async_perform(&::aio_write);
    }
    int fsync() {
        return (int)async_perform(&::aio_fsync, O_SYNC);
    }
    int fdatasync() {
        return (int)async_perform(&::aio_fsync, O_DSYNC);
    }
};
class Counter {
public:
    int &c;
    Counter(int &c) : c(c) {
        c++;
    }
    ~Counter() {
        c--;
    }
};
ssize_t libaio_pread(int fd, void *buf, size_t count, off_t offset) {
    static int n;
    Counter c(n);
    return libaiocb().asyncio(&io_prep_pread, fd, buf, count, offset);
}
ssize_t libaio_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
    static int n;
    Counter c(n);
    return libaiocb().asyncio(&io_prep_preadv, fd, iov, iovcnt, offset);
}
ssize_t libaio_pwrite(int fd, const void *buf, size_t count, off_t offset) {
    static int n;
    Counter c(n);
    return libaiocb().asyncio(&io_prep_pwrite, fd, (void *)buf, count, offset);
}
ssize_t libaio_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
    static int n;
    Counter c(n);
    return libaiocb().asyncio(&io_prep_pwritev, fd, iov, iovcnt, offset);
}

ssize_t posixaio_pread(int fd, void *buf, size_t count, off_t offset) {
    static int n;
    Counter c(n);
    return posix_aiocb(fd).pread(buf, count, offset);
}
ssize_t posixaio_pwrite(int fd, const void *buf, size_t count, off_t offset) {
    static int n;
    Counter c(n);
    return posix_aiocb(fd).pwrite((void *)buf, count, offset);
}
int posixaio_fsync(int fd) {
    static int n;
    Counter c(n);
    return posix_aiocb(fd).fsync();
}
int posixaio_fdatasync(int fd) {
    static int n;
    Counter c(n);
    return posix_aiocb(fd).fdatasync();
}
ssize_t posixaio::preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
    ssize_t rst = 0;
    for (auto &x : ptr_array(iov, iovcnt)) {
        ssize_t ret = posixaio_pread(fd, x.iov_base, x.iov_len, offset + rst);
        if (ret < 0)
            LOG_ERRNO_RETURN(-1, 0, "failed to posixaio_preadv");
        if (ret < (ssize_t)x.iov_len)
            return rst + ret;
        rst += ret;
    }
    return rst;
}
ssize_t posixaio::pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
    ssize_t rst = 0;
    for (auto &x : ptr_array(iov, iovcnt)) {
        ssize_t ret = posixaio_pwrite(fd, x.iov_base, x.iov_len, offset + rst);
        if (ret < 0)
            LOG_ERRNO_RETURN(-1, 0, "failed to posixaio_pwrite()");
        if (ret < (ssize_t)x.iov_len)
            return rst + ret;
        rst += ret;
    }
    return rst;
}

int libaio_wrapper_init() {
    if (evfd != 0)
        LOG_ERROR_RETURN(EALREADY, -1, "already inited");

    evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evfd < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to create eventfd");

    int ret = io_setup(IODEPTH, &aio_ctx);
    if (ret < 0) {
        LOG_ERROR("failed to create aio context by io_setup() ", ERRNO());
        close(evfd);
        return ret;
    }

    polling_thread = thread_create(&libaio_polling, nullptr);
    if (!polling_thread) { /* should not fail */
    }
    return 0;
}

int libaio_wrapper_fini() {
    if (!running || !polling_thread || !evfd)
        LOG_ERROR_RETURN(ENOSYS, -1, "not inited");

    if (running == 2) // if waiting for fd readable
        thread_interrupt(polling_thread, ECANCELED);

    running = -1;
    while (running != 0)
        thread_usleep(1000 * 10);

    io_destroy(aio_ctx);
    close(evfd);
    evfd = 0;
    return 0;
}
} // namespace photon
