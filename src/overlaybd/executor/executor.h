/*
 * executor.h
 *
 * Copyright (C) 2021 Alibaba Group.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */

#pragma once

#include "../alog.h"
#include "../event-loop.h"
#include "../photon/syncio/fd-events.h"
#include "../photon/thread-pool.h"
#include "../photon/thread.h"
#include "../utility.h"
#include "stdlock.h"
#include <atomic>
#include <boost/lockfree/queue.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Executor {

constexpr int64_t kCondWaitMaxTime = 1000L * 1000;
constexpr int64_t kCondWaitStepTime = 10L * 1000;

template <typename R, typename Context>
struct AsyncReturn {
    R result;
    std::atomic_bool gotit;
    typename Context::Mutex mtx;
    typename Context::Cond cond;
    AsyncReturn() : gotit(false), cond(mtx) {
    }
    R wait_for_result() {
        typename Context::CondLock lock(mtx);
        while (!gotit) {
            cond.wait_for(lock, kCondWaitMaxTime);
        }
        return result;
    }
    void set_result(R r) {
        typename Context::CondLock lock(mtx);
        result = std::forward<R>(r);
        gotit = true;
        cond.notify_all();
    }
};

template <typename Context>
struct AsyncReturn<void, Context> {
    std::atomic_bool gotit;
    typename Context::Mutex mtx;
    typename Context::Cond cond;
    AsyncReturn() : gotit(false), cond(mtx) {
    }
    void wait_for_result() {
        typename Context::CondLock lock(mtx);
        while (!gotit) {
            cond.wait_for(lock, kCondWaitMaxTime);
        }
    }
    void set_result() {
        typename Context::CondLock lock(mtx);
        gotit = true;
        cond.notify_all();
    }
};

struct YieldOp {
    static void yield() {
        ::sched_yield();
    }
};

class HybridEaseExecutor {
public:
    template <typename R, typename Context>
    using AsyncReturnP = typename std::shared_ptr<AsyncReturn<R, Context>>;

    HybridEaseExecutor() : ack(0) {
        loop = new_event_loop({this, &HybridEaseExecutor::wait_for_event},
                              {this, &HybridEaseExecutor::on_event});
        th = new std::thread(&HybridEaseExecutor::do_loop, this);
        while (!loop || loop->state() != loop->WAITING)
            ::sched_yield();
    }

    ~HybridEaseExecutor() {
        photon::safe_thread_interrupt(pth);
        th->join();
        delete th;
    }

    template <typename Context = StdContext, typename Func,
              typename R = typename std::result_of<Func()>::type,
              typename = typename std::enable_if<!std::is_same<R, void>::value>::type>
    R perform(Func &&act) {
        auto aret = new AsyncReturn<R, Context>();
        DEFER(delete aret);
        auto work = [act, aret, this] {
            if (!aret->gotit) {
                aret->set_result(act());
            }
            return 0;
        };
        Callback<> cb(work);
        issue(cb);
        return aret->wait_for_result();
    }

    template <typename Context = StdContext, typename Func,
              typename R = typename std::result_of<Func()>::type,
              typename = typename std::enable_if<!std::is_same<R, void>::value>::type>
    AsyncReturnP<R, Context> async_perform(Func &&act) {
        auto aret = new AsyncReturn<AsyncReturnP<R, Context>, Context>();
        DEFER(delete aret);
        auto work = [act, aret, this] {
            auto arp = std::make_shared<AsyncReturn<R, Context>>();
            aret->set_result(arp);
            if (!arp->gotit) {
                arp->set_result(act());
            }
            return 0;
        };
        Callback<> cb(work);
        issue(cb);
        return aret->wait_for_result();
    }

    template <typename Context = StdContext, typename Func,
              typename R = typename std::result_of<Func()>::type,
              typename = typename std::enable_if<std::is_same<R, void>::value>::type>
    void perform(Func &&act) {
        auto aret = new AsyncReturn<void, Context>();
        DEFER(delete aret);
        auto work = [act, aret, this] {
            if (!aret->gotit) {
                act();
                aret->set_result();
            }
            return 0;
        };
        Callback<> cb(work);
        issue(cb);
        return aret->wait_for_result();
    }

    template <typename Context = StdContext, typename Func,
              typename R = typename std::result_of<Func()>::type,
              typename = typename std::enable_if<std::is_same<R, void>::value>::type>
    AsyncReturnP<void, Context> async_perform(Func &&act) {
        auto aret = new AsyncReturn<AsyncReturnP<void, Context>, Context>();
        DEFER(delete aret);
        auto work = [act, aret, this] {
            auto arp = std::make_shared<AsyncReturn<void, Context>>();
            aret->set_result(arp);
            if (!arp->gotit) {
                act();
                arp->set_result();
            }
            return 0;
        };
        Callback<> cb(work);
        issue(cb);
        return aret->wait_for_result();
    }

protected:
    using CBList =
        typename boost::lockfree::queue<Callback<>, boost::lockfree::capacity<32UL * 1024>>;
    std::thread *th = nullptr;
    photon::thread *pth = nullptr;
    EventLoop *loop = nullptr;
    CBList queue;
    std::atomic<int> ack;
    photon::ThreadPoolBase *pool;

    void issue(Callback<> act) {
        while (!queue.push(act))
            YieldOp::yield();
        photon::safe_thread_interrupt(loop->loop_thread(), EINPROGRESS);
    }

    int wait_for_event(EventLoop *) {
        if (queue.empty()) {
            // prevent sleep too long
            int ret = photon::thread_usleep(kCondWaitMaxTime);
            if (ret < 0) {
                ERRNO err;
                if (err.no == EINPROGRESS)
                    return 1;
                else if (err.no == EINTR)
                    return -1;
            }
            return 0;
        }
        return 1;
    }

    static void *do_event(void *arg) {
        auto a = (Callback<> *)arg;
        auto task = *a;
        delete a;
        task();
        return nullptr;
    }

    int on_event(EventLoop *) {
        while (!queue.empty()) {
            auto args = new Callback<>;
            auto &task = *args;
            if (queue.pop(task)) {
                pool->thread_create(&HybridEaseExecutor::do_event, (void *)args);
            }
        }
        return 0;
    }

    void do_loop() {
        photon::init();
        photon::fd_events_init();
        pth = photon::CURRENT;
        LOG_INFO("worker start");
        pool = photon::new_thread_pool(32);
        loop->async_run();
        photon::thread_usleep(-1);
        LOG_INFO("worker finished");
        while (!queue.empty())
            photon::thread_usleep(1000 * 100);
        delete loop;
        photon::delete_thread_pool(pool);
        pool = nullptr;
        photon::fd_events_fini();
        photon::fini();
    }
};
} // namespace Executor