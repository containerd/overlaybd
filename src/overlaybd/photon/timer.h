/*
 * timer.h
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
#include <inttypes.h>
#include <assert.h>
#include "thread.h"
#include "../callback.h"

namespace photon {
class Timer {
public:
    // the prototype of timer entry function
    // return value will be used as the next timeout,
    // 0 for default_timeout (given in the ctor)
    using Entry = Delegate<uint64_t>;

    // Create a timer object with `default_timedout` in usec, callback function `on_timer`,
    // and callback argument `arg`. The timer object is implemented as a special thread, so
    // it has a `stack_size`, and the `on_timer` is invoked within the thread's context.
    // The timer object is deleted automatically after it is finished.
    Timer(uint64_t default_timeout, Entry on_timer, bool repeating = true,
          uint64_t stack_size = 1024 * 64) {
        _on_timer = on_timer;
        _default_timeout = default_timeout;
        _repeating = repeating;
        _th = thread_create(&_stub, this, stack_size);
        thread_enable_join(_th);
        thread_yield_to(_th);
    }
    // reset the timer's timeout
    int reset(uint64_t new_timeout = -1) {
        if (!_waiting) {
            return -1;
        }
        _reset_timeout = new_timeout;
        thread_interrupt(_th, EAGAIN);
        thread_yield_to(_th);
        return 0;
    }
    int cancel() {
        return reset(-1);
    }
    int stop() {
        while (cancel())
            _wait_ready.wait_no_lock();
        return 0;
    }
    ~Timer() {
        if (!_th)
            return;
        stop();
        _repeating = false;
        if (_waiting)
            thread_interrupt(_th, ECANCELED);

        // wait for the worker thread to complete
        thread_join((join_handle *)_th);
    }

protected:
    thread *_th;
    Entry _on_timer;
    uint64_t _default_timeout;
    uint64_t _reset_timeout;
    bool _repeating;
    bool _waiting = false;
    condition_variable _wait_ready;
    static void *_stub(void *_this);
    void stub();
};
} // namespace photon
