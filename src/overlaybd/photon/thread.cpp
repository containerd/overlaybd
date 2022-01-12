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
#define protected public
#include "thread.h"
#include "timer.h"
#undef protected
#include <memory.h>
#include <assert.h>
#include <errno.h>
#include <vector>
#include <new>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include <sys/time.h>
#include "list.h"
#include "../alog.h"

namespace photon {
static std::condition_variable idle_sleep;
static int default_idle_sleeper(uint64_t usec) {
    static std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    auto us =
        ((int64_t &)usec) < 0 ? std::chrono::microseconds::max() : std::chrono::microseconds(usec);
    idle_sleep.wait_for(lock, us);
    return 0;
}
IdleSleeper idle_sleeper = &default_idle_sleeper;
void set_idle_sleeper(IdleSleeper sleeper) {
    idle_sleeper = sleeper ? sleeper : &default_idle_sleeper;
}
IdleSleeper get_idle_sleeper() {
    return idle_sleeper;
}

class Stack {
public:
    template <typename F>
    void init(void *ptr, F ret2func) {
        _ptr = ptr;
        push(0);
        push(ret2func);
        (uint64_t *&)_ptr -= 6;
    }

    void **pointer_ref() {
        return &_ptr;
    }
    void push(uint64_t x) {
        *--(uint64_t *&)_ptr = x;
    }
    template <typename T>
    void push(const T &x) {
        push((uint64_t)x);
    }
    uint64_t pop() {
        return *((uint64_t *&)_ptr)++;
    }
    uint64_t &operator[](int i) {
        return static_cast<uint64_t *>(_ptr)[i];
    }
    void *_ptr;
};

struct thread;
typedef intrusive_list<thread> thread_list;
struct thread : public intrusive_list_node<thread> {
    states state = states::READY;
    int error_number = 0;
    int idx; /* index in the sleep queue array */
    int flags = 0;
    int reserved;
    bool joinable = false;
    bool shutting_down = false; // the thread should cancel what is doing, and quit
                                // current job ASAP; not allowed to sleep or block more
                                // than 10ms, otherwise -1 will be returned and errno == EPERM

    thread_list *waitq = nullptr; /* the q if WAITING in a queue */

    thread_entry start;
    void *arg;
    void *retval;
    void *go() {
        auto _arg = arg;
        arg = nullptr; // arg will be used as thread-local variable
        return retval = start(_arg);
    }
    char *buf;

    Stack stack;
    uint64_t ts_wakeup = 0;  /* Wakeup time when thread is sleeping */
    condition_variable cond; /* used for join, or timer REUSE */

    int set_error_number() {
        if (error_number) {
            errno = error_number;
            error_number = 0;
            return -1;
        }
        return 0;
    }

    void dequeue_ready() {
        if (waitq) {
            waitq->erase(this);
            waitq = nullptr;
        } else {
            assert(this->single());
        }
        state = states::READY;
        CURRENT->insert_tail(this);
    }

    bool operator<(const thread &rhs) {
        return this->ts_wakeup < rhs.ts_wakeup;
    }

    void dispose() {
        delete[] buf;
    }
};

class SleepQueue {
public:
    std::vector<thread *> q;

    thread *front() const {
        return q.empty() ? nullptr : q.front();
    }

    bool empty() const {
        return q.empty();
    }

    int push(thread *obj) {
        q.push_back(obj);
        obj->idx = q.size() - 1;
        up(obj->idx);
        return 0;
    }

    thread *pop_front() {
        auto ret = q[0];
        q[0] = q.back();
        q[0]->idx = 0;
        q.pop_back();
        down(0);
        return ret;
    }

    int pop(thread *obj) {
        if (obj->idx == -1)
            return -1;
        if ((size_t)obj->idx == q.size() - 1) {
            q.pop_back();
            obj->idx = -1;
            return 0;
        }

        auto id = obj->idx;
        q[obj->idx] = q.back();
        q[id]->idx = id;
        q.pop_back();
        if (!up(id))
            down(id);
        obj->idx = -1;

        return 0;
    }

    __attribute__((always_inline)) void update_node(int idx, thread *&obj) {
        q[idx] = obj;
        q[idx]->idx = idx;
    }

    // compare m_nodes[idx] with parent node.
    bool up(int idx) {
        auto tmp = q[idx];
        bool ret = false;
        while (idx != 0) {
            auto cmpIdx = (idx - 1) >> 1;
            if (*tmp < *q[cmpIdx]) {
                update_node(idx, q[cmpIdx]);
                idx = cmpIdx;
                ret = true;
                continue;
            }
            break;
        }
        if (ret)
            update_node(idx, tmp);
        return ret;
    }

    // compare m_nodes[idx] with child node.
    bool down(int idx) {
        auto tmp = q[idx];
        size_t cmpIdx = (idx << 1) + 1;
        bool ret = false;
        while (cmpIdx < q.size()) {
            if (cmpIdx + 1 < q.size() && *q[cmpIdx + 1] < *q[cmpIdx])
                cmpIdx++;
            if (*q[cmpIdx] < *tmp) {
                update_node(idx, q[cmpIdx]);
                idx = cmpIdx;
                cmpIdx = (idx << 1) + 1;
                ret = true;
                continue;
            }
            break;
        }
        if (ret)
            update_node(idx, tmp);
        return ret;
    }
};

thread *CURRENT = new thread;
static SleepQueue sleepq;

static void thread_die(thread *th) {
    th->dispose();
}

void photon_die_and_jmp_to_context(thread *dying_th, void **dest_context,
                                   void (*th_die)(thread *)) asm("_photon_die_and_jmp_to_context");
static int do_idle_sleep(uint64_t usec);
static int resume_sleepers();
static inline void switch_context(thread *from, states new_state, thread *to);

static void enqueue_wait(thread_list *q, thread *th, uint64_t expire) {
    assert(th->waitq == nullptr);
    th->ts_wakeup = expire;
    if (q) {
        q->push_back(th);
        th->waitq = q;
    }
}

static void thread_stub() {
    CURRENT->go();
    CURRENT->cond.notify_all();
    while (CURRENT->single() && !sleepq.empty()) {
        if (resume_sleepers() == 0)
            do_idle_sleep(-1);
    }

    auto th = CURRENT;
    CURRENT = CURRENT->remove_from_list();
    if (!th->joinable) {
        th->state = states::DONE;
        photon_die_and_jmp_to_context(th, CURRENT->stack.pointer_ref(), &thread_die);
    } else {
        switch_context(th, states::DONE, CURRENT);
    }
}

thread *thread_create(void *(*start)(void *), void *arg, uint64_t stack_size) {
    stack_size += (rand() % 32) * (1024 + 8);
    auto ptr = new char[stack_size];
    auto p = ptr + stack_size - sizeof(thread);
    (uint64_t &)p &= ~63;
    auto th = new (p) thread;
    th->buf = ptr;
    th->idx = -1;
    th->start = start;
    th->arg = arg;
    th->stack.init(p, &thread_stub);
    th->state = states::READY;
    CURRENT->insert_tail(th);
    return th;
}

uint64_t now;
static inline uint64_t update_now() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    now = tv.tv_sec;
    now *= 1000 * 1000;
    return now += tv.tv_usec;
}
static inline void prefetch_context(thread *from, thread *to) {
#ifdef CONTEXT_PREFETCHING
    const int CACHE_LINE_SIZE = 64;
    auto f = *from->stack.pointer_ref();
    __builtin_prefetch(f, 1);
    __builtin_prefetch((char *)f + CACHE_LINE_SIZE, 1);
    auto t = *to->stack.pointer_ref();
    __builtin_prefetch(t, 0);
    __builtin_prefetch((char *)t + CACHE_LINE_SIZE, 0);
#endif
}
extern void photon_switch_context(void **, void **) asm("_photon_switch_context");
static inline void switch_context(thread *from, states new_state, thread *to) {
    from->state = new_state;
    to->state = states::RUNNING;
    photon_switch_context(from->stack.pointer_ref(), to->stack.pointer_ref());
}

static int resume_sleepers() {
    int count = 0;
    update_now();
    while (true) {
        auto th = sleepq.front();
        if (!th || now < th->ts_wakeup)
            break;

        sleepq.pop_front();
        th->dequeue_ready();
        count++;
    }
    return count;
}

states thread_stat(thread *th) {
    return th->state;
}

void thread_yield() {
    if (CURRENT->single()) {
        auto ret = resume_sleepers(); // photon::now will be update during resume
        if (ret == 0)
            return; // no target to yield to
    } else {
        update_now(); // or update photon::now here
    }

    auto t0 = CURRENT;
    CURRENT = CURRENT->next();
    prefetch_context(t0, CURRENT);
    switch_context(t0, states::READY, CURRENT);
}

void thread_yield_to(thread *th) {
    if (th == nullptr) // yield to any thread
    {
        if (CURRENT->single()) {
            auto ret = resume_sleepers(); // photon::now will be update
            if (ret == 0)
                return; // no target to yield to
        }
        th = CURRENT->next(); // or not update
    } else if (th->state != states::READY) {
        LOG_ERROR_RETURN(EINVAL, , VALUE(th), " must be READY!");
    }

    auto t0 = CURRENT;
    CURRENT = th;
    prefetch_context(t0, CURRENT);
    switch_context(t0, states::READY, CURRENT);
}

static int do_idle_sleep(uint64_t usec) {
    if (!sleepq.empty())
        if (sleepq.front()->ts_wakeup > now)
            usec = std::min(usec, sleepq.front()->ts_wakeup - now);
    auto ret = idle_sleeper(usec);
    update_now();
    return ret;
}

// returns 0 if slept well (at lease `useconds`), -1 otherwise
static int thread_usleep(uint64_t useconds, thread_list *waitq) {
    if (useconds == 0) {
        thread_yield();
        return 0;
    }
    CURRENT->state = states::WAITING;
    auto expire = sat_add(now, useconds);
    while (CURRENT->single()) // if no active threads available
    {
        if (resume_sleepers() > 0) // will update_now() in it
        {
            break;
        }
        if (now >= expire) {
            return 0;
        }
        do_idle_sleep(useconds);
        if (CURRENT->state == states::READY) // CURRENT has been woken up during idle sleep
        {
            CURRENT->set_error_number();
            return -1;
        }
    }

    auto t0 = CURRENT;
    CURRENT = CURRENT->remove_from_list();
    prefetch_context(t0, CURRENT);
    assert(CURRENT != nullptr);
    enqueue_wait(waitq, t0, expire);
    sleepq.push(t0);
    switch_context(t0, states::WAITING, CURRENT);
    return t0->set_error_number();
}

int thread_usleep(uint64_t useconds) {
    if (CURRENT->shutting_down && useconds > 10 * 1000) {
        int ret = thread_usleep(10 * 1000, nullptr);
        if (ret >= 0)
            errno = EPERM;
        return -1;
    }
    return thread_usleep(useconds, nullptr);
}

void thread_interrupt(thread *th, int error_number) {
    if (th->state == states::READY) { // th is already in runing queue
        return;
    }

    if (!th || th->state != states::WAITING)
        LOG_ERROR_RETURN(EINVAL, , "invalid parameter");

    if (th ==
        CURRENT) { // idle_sleep may run in CURRENT's context, which may be single() and WAITING
        th->state = states::READY;
        th->error_number = error_number;
        return;
    }
    sleepq.pop(th);
    th->dequeue_ready();
    th->error_number = error_number;
}

join_handle *thread_enable_join(thread *th, bool flag) {
    th->joinable = flag;
    return (join_handle *)th;
}

void thread_join(join_handle *jh) {
    auto th = (thread *)jh;
    if (!th->joinable)
        LOG_ERROR_RETURN(ENOSYS, , "join is not enabled for thread ", th);

    if (th->state != states::DONE) {
        th->cond.wait_no_lock();
        th->remove_from_list();
    }
    th->dispose();
}

int thread_shutdown(thread *th, bool flag) {
    if (!th)
        LOG_ERROR_RETURN(EINVAL, -1, "invalid thread");

    th->shutting_down = flag;
    if (th->state == states::WAITING)
        thread_interrupt(th, EPERM);
    return 0;
}

void *thread_get_local() {
    return CURRENT->arg;
}
void thread_set_local(void *local) {
    CURRENT->arg = local;
}

void Timer::stub() {
    auto timeout = _default_timeout;
    do {
    again:
        _waiting = true;
        _wait_ready.notify_all();
        int ret = thread_usleep(timeout);
        _waiting = false;
        if (ret < 0) {
            int e = errno;
            if (e == ECANCELED) {
                break;
            } else if (e == EAGAIN) {
                timeout = _reset_timeout;
                goto again;
            } else
                assert(false);
        }

        timeout = _on_timer.fire();
        if (!timeout)
            timeout = _default_timeout;
    } while (_repeating);
    _th = nullptr;
}
void *Timer::_stub(void *_this) {
    static_cast<Timer *>(_this)->stub();
    return nullptr;
}

int waitq::wait(uint64_t timeout) {
    static_assert(sizeof(q) == sizeof(thread_list), "...");
    int ret = thread_usleep(timeout, (thread_list *)&q);
    if (ret == 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    return (errno == ECANCELED) ? 0 : -1;
}
void waitq::resume(thread *th) {
    assert(th->waitq == (thread_list *)&q);
    if (!th || !q || th->waitq != (thread_list *)&q)
        return;
    // will update q during thread_interrupt()
    thread_interrupt(th, ECANCELED);
}
int mutex::lock(uint64_t timeout) {
    if (owner == CURRENT)
        LOG_ERROR_RETURN(EINVAL, -1, "recursive locking is not supported");

    while (owner) {
        if (wait(timeout) < 0) {
            // EINTR means break waiting without holding lock
            // it is normal in OutOfOrder result collected situation, and that is the only
            // place using EINTR to interrupt micro-threads (during getting lock)
            // normally, ETIMEOUT means wait timeout and ECANCELED means resume from sleep
            // LOG_DEBUG("timeout return -1;", VALUE(CURRENT), VALUE(this), VALUE(owner));
            return -1; // timedout or interrupted
        }
    }

    owner = CURRENT;
    return 0;
}
void mutex::unlock() {
    if (owner != CURRENT)
        return;

    owner = nullptr;
    resume_one();
}
int recursive_mutex::lock(uint64_t timeout) {
    if (owner == CURRENT || mutex::lock(timeout) == 0) {
        recursive_count++;
        return 0;
    }
    return -1;
}
int recursive_mutex::try_lock() {
    if (owner == CURRENT || mutex::try_lock() == 0) {
        recursive_count++;
        return 0;
    }
    return -1;
}
void recursive_mutex::unlock() {
    if (owner != CURRENT)
        return;
    if (--recursive_count > 0) {
        return;
    }
    mutex::unlock();
}
int condition_variable::wait_no_lock(uint64_t timeout) {
    return waitq::wait(timeout);
}
int condition_variable::wait(scoped_lock &lock,
                             uint64_t timeout) { // current implemention is only for interface
                                                 // compatibility, needs REDO for multi-vcpu
    if (!lock.locked())
        return wait_no_lock(timeout);

    lock.unlock();
    int ret = wait_no_lock(timeout);
    lock.lock();
    return ret;
}
void condition_variable::notify_one() {
    resume_one();
}
void condition_variable::notify_all() {
    while (q)
        resume_one();
}
int semaphore::wait(uint64_t count, uint64_t timeout) {
    if (count == 0)
        return 0;
    while (m_count < count) {
        CURRENT->retval = (void *)count;
        int ret = waitq::wait(timeout);
        if (ret < 0) {
            // when timeout, and CURRENT was the first in waitq,
            // we need to try to resume the next one in q
            signal(0);
            return -1;
        }
    }
    m_count -= count;
    return 0;
}
int semaphore::signal(uint64_t count) {
    m_count += count;
    while (q) {
        auto q_front_count = (uint64_t)q->retval;
        if (m_count < q_front_count)
            break;
        resume_one();
    }
    return 0;
}
int rwlock::lock(int mode, uint64_t timeout) {
    if (mode != RLOCK && mode != WLOCK)
        LOG_ERROR_RETURN(EINVAL, -1, "mode unknow");
    // backup retval
    void *bkup = CURRENT->retval;
    DEFER(CURRENT->retval = bkup);
    auto mark = (uint64_t)CURRENT->retval;
    // mask mark bits, keep RLOCK WLOCK bit clean
    mark &= ~(RLOCK | WLOCK);
    // mark mode and set as retval
    mark |= mode;
    CURRENT->retval = (void *)(mark);
    int op;
    if (mode == RLOCK) {
        op = 1;
    } else { // WLOCK
        op = -1;
    }
    if (q || (op == 1 && state < 0) || (op == -1 && state > 0)) {
        do {
            int ret = wait(timeout);
            if (ret < 0)
                return -1; // break by timeout or interrupt
        } while ((op == 1 && state < 0) || (op == -1 && state > 0));
    }
    state += op;
    return 0;
}
int rwlock::unlock() {
    assert(state != 0);
    if (state > 0)
        state--;
    else
        state++;
    if (state == 0 && q) {
        if (((uint64_t)q->retval) & WLOCK)
            resume_one();
        else
            while (q && (((uint64_t)q->retval) & RLOCK))
                resume_one();
    }
    return 0;
}

int init() {
    CURRENT->idx = -1;
    CURRENT->state = states::RUNNING;
    update_now();
    return 0;
}
int fini() {
    return 0;
}
} // namespace photon
