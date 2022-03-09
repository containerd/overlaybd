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
#include <inttypes.h>
#include <assert.h>
#include <errno.h>

namespace photon {
int init();
int fini();

struct timer;
struct thread;
extern thread *CURRENT;
extern uint64_t now;

enum states {
    READY = 0,   // ready to run
    RUNNING = 1, // CURRENTly running
    WAITING = 2, // waiting for some events
    DONE = 4,    // finished the whole life-cycle
};

typedef void *(*thread_entry)(void *);
const uint64_t DEFAULT_STACK_SIZE = 8 * 1024 * 1024;
thread *thread_create(thread_entry start, void *arg, uint64_t stack_size = DEFAULT_STACK_SIZE);

// Threads are join-able *only* through their join_handle.
// Once join is enabled, the thread will remain existing until being joined.
// Failing to do so will cause resource leak.
struct join_handle;
join_handle *thread_enable_join(thread *th, bool flag = true);
void thread_join(join_handle *jh);

// switching to other threads (without going into sleep queue)
void thread_yield();

// switching to a specific thread, which must be RUNNING
void thread_yield_to(thread *th);

// suspend CURRENT thread for specified time duration, and switch
// control to other threads, resuming possible sleepers
int thread_usleep(uint64_t useconds);
inline int thread_sleep(uint64_t seconds) {
    const uint64_t max_seconds = ((uint64_t)-1) / 1000 / 1000;
    uint64_t usec = (seconds >= max_seconds ? -1 : seconds * 1000 * 1000);
    return thread_usleep(usec);
}

inline void thread_suspend() {
    thread_usleep(-1);
}

states thread_stat(thread *th = CURRENT);
void thread_interrupt(thread *th, int error_number = EINTR);
inline void thread_resume(thread *th) {
    thread_interrupt(th, 0);
}

// if true, the thread `th` should cancel what is doing, and quit
// current job ASAP (not allowed `th` to sleep or block more than
// 10ms, otherwise -1 will be returned to `th` and errno == EPERM;
// if it is currently sleeping or blocking, it is thread_interupt()ed
// with EPERM)
int thread_shutdown(thread *th, bool flag = true);

// the getter and setter of thread-local variable
// getting and setting local in a timer context will cause undefined behavior!
void *thread_get_local();
void thread_set_local(void *local);

class waitq {
public:
    int wait(uint64_t timeout = -1);
    void resume(thread *th); // `th` must be waiting in this waitq!
    void resume_one() {
        if (q)
            resume(q);
    }
    waitq() = default;
    waitq(const waitq &rhs) = delete; // not allowed to copy construct
    waitq(waitq &&rhs) {
        q = rhs.q;
        rhs.q = nullptr;
    }
    ~waitq() {
        assert(q == nullptr);
    }

protected:
    thread *q = nullptr; // the first thread in queue, if any
};

class mutex : protected waitq {
public:
    void unlock();
    int lock(uint64_t timeout = -1);    // threads are guaranteed to get the lock
    int try_lock(uint64_t timeout = -1) // in FIFO order, when there's contention
    {
        return owner ? -1 : lock();
    }
    ~mutex() {
        assert(owner == nullptr);
    }

protected:
    thread *owner = nullptr;
};

class recursive_mutex : protected mutex {
public:
    int lock(uint64_t timeout = -1);
    int try_lock();
    void unlock();

protected:
    int32_t recursive_count = 0;
};

class scoped_lock {
public:
    // do lock() if `do_lock` > 0, and lock() can NOT fail if `do_lock` > 1
    explicit scoped_lock(mutex &mutex, uint64_t do_lock = 2) : m_mutex(&mutex) {
        if (do_lock > 0) {
            lock(do_lock > 1);
        } else {
            // assert(!m_locked);
            m_locked = false;
        }
    }
    scoped_lock(scoped_lock &&rhs) : m_mutex(rhs.m_mutex) {
        m_locked = rhs.m_locked;
        rhs.m_mutex = nullptr;
        rhs.m_locked = false;
    }
    scoped_lock(const scoped_lock &rhs) = delete;
    int lock(bool must_lock = true) {
        int ret;
        do {
            ret = m_mutex->lock();
            m_locked = (ret == 0);
        } while (!m_locked && must_lock);
        return ret;
    }
    int try_lock() {
        auto ret = m_mutex->try_lock();
        m_locked = (ret == 0);
        return ret;
    };
    bool locked() {
        return m_locked;
    }
    operator bool() {
        return locked();
    }
    void unlock() {
        if (m_locked) {
            m_mutex->unlock();
            m_locked = false;
        }
    }
    ~scoped_lock() {
        if (m_locked)
            m_mutex->unlock();
    }
    void operator=(const scoped_lock &rhs) = delete;
    void operator=(scoped_lock &&rhs) = delete;

protected:
    mutex *m_mutex;
    bool m_locked;
};

class condition_variable : protected waitq {
public:
    int wait(scoped_lock &lock, uint64_t timeout = -1);
    int wait_no_lock(uint64_t timeout = -1);
    void notify_one();
    void notify_all();
};

class semaphore : protected waitq // NOT TESTED YET
{
public:
    explicit semaphore(uint64_t count = 0) : m_count(count) {
    }
    int wait(uint64_t count, uint64_t timeout = -1);
    int signal(uint64_t count);

protected:
    uint64_t m_count;
};

// to be different to timer flags
// mark flag should be larger than 999, and not touch lower bits
// here we selected
constexpr int RLOCK = 0x1000;
constexpr int WLOCK = 0x2000;
class rwlock : protected waitq {
public:
    int lock(int mode, uint64_t timeout = -1);
    int unlock();

protected:
    int64_t state = 0;
};

class scoped_rwlock {
public:
    scoped_rwlock(rwlock &rwlock, int lockmode) : m_locked(false) {
        m_rwlock = &rwlock;
        m_locked = (0 == m_rwlock->lock(lockmode));
    }
    bool locked() {
        return m_locked;
    }
    operator bool() {
        return locked();
    }
    int lock(int mode, bool must_lock = true) {
        int ret;
        do {
            ret = m_rwlock->lock(mode);
            m_locked = (0 == ret);
        } while (!m_locked && must_lock);
        // return 0 for locked else return -1 means failure
        return ret;
    }
    ~scoped_rwlock() {
        // only unlock when it is actually locked
        if (m_locked)
            m_rwlock->unlock();
    }

protected:
    rwlock *m_rwlock;
    bool m_locked;
};

template <uint64_t N = 8>
inline void threads_create_join(uint64_t n, thread_entry start, void *arg,
                                uint64_t stack_size = DEFAULT_STACK_SIZE) {
    uint64_t i;
    join_handle *jh[N];
    if (n > N)
        n = N;
    for (i = 0; i < n; ++i) {
        auto th = thread_create(start, arg, stack_size);
        if (!th)
            break;
        jh[i] = thread_enable_join(th);
    }
    for (; i; --i) {
        thread_join(jh[i - 1]);
    }
}

// `usec` is the *maximum* amount of time to sleep
//  returns 0 if slept well or interrupted by IdleWakeUp() or qlen
//  returns -1 error occured with in IdleSleeper()
//  Do NOT invoke photon::usleep() or photon::sleep() in IdleSleeper,
//  because their implementation depends on IdleSleeper.
typedef int (*IdleSleeper)(uint64_t usec);
void set_idle_sleeper(IdleSleeper idle_sleeper);
IdleSleeper get_idle_sleeper();

// Saturating addition, primarily for timeout caculation
__attribute__((always_inline)) inline uint64_t sat_add(uint64_t x, uint64_t y) {
    register uint64_t z asm("rax");
    asm("add %2, %1; sbb %0, %0; or %1, %0;" : "=r"(z), "+r"(x) : "r"(y) : "cc");
    return z;
}

// Saturating subtract, primarily for timeout caculation
__attribute__((always_inline)) inline uint64_t sat_sub(uint64_t x, uint64_t y) {
    return x > y ? x - y : 0;
}
}; // namespace photon

/*
 WITH_LOCK(mutex)
 {
    ...
 }
*/
#define WITH_LOCK(mutex) if (auto __lock__ = scoped_lock(mutex))
