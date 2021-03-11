/*
 * thread-poll.cpp
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
#include "thread-pool.h"
#include "../alog.h"

namespace photon {
TPControl *ThreadPoolBase::thread_create_ex(thread_entry start, void *arg, bool joinable) {
    auto pCtrl = B::get();
    pCtrl->joinable = joinable;
    pCtrl->start = start;
    pCtrl->arg = arg;
    pCtrl->cvar.notify_one();
    return pCtrl;
}
void *ThreadPoolBase::stub(void *arg) {
    TPControl ctrl;
    auto th = *(thread **)arg;
    *(TPControl **)arg = &ctrl; // tell ctor where my `ctrl` is
    thread_yield_to(th);
    while (true) {
        while (!ctrl.start)           // wait for `create()` to give me
            ctrl.cvar.wait_no_lock(); // thread_entry and argument

        if (ctrl.start == &stub)
            break;
        thread_set_local(nullptr);
        ctrl.start(ctrl.arg);
        ctrl.cvar.notify_all();
        ctrl.start = nullptr;
        ctrl.pool->put(&ctrl);
    }
    if (ctrl.joinable) // wait for being joined
        ctrl.cvar.wait_no_lock();
    return nullptr;
}
void ThreadPoolBase::join(TPControl *pCtrl) {
    if (!pCtrl->joinable)
        LOG_ERROR_RETURN(EINVAL, , "thread is not joinable");

    if (pCtrl->start == &stub) // a dying thread
    {
        pCtrl->cvar.notify_all();
        return;
    }

    while (pCtrl->start && pCtrl->start != &stub)
        pCtrl->cvar.wait_no_lock();
}
int ThreadPoolBase::ctor(ThreadPoolBase *pool, TPControl **out) {
    auto pCtrl = (TPControl *)CURRENT;
    auto stack_size = (uint64_t)pool->m_reserved;
    auto th = photon::thread_create(&stub, &pCtrl, stack_size);
    thread_yield_to(th);
    assert(pCtrl);
    *out = pCtrl;
    pCtrl->th = th;
    pCtrl->pool = pool;
    pCtrl->start = nullptr;
    return 0;
}
int ThreadPoolBase::dtor(ThreadPoolBase *, TPControl *pCtrl) {
    pCtrl->start = &stub;
    pCtrl->cvar.notify_all();
    return 0;
}
} // namespace photon
