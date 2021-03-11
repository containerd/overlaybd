/*
 * event-loop.h
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
#include "callback.h"
#include "object.h"

namespace photon {
class thread;
};

class EventLoop : public Object {
public:
    const static int STOP = 0;
    const static int RUNNING = 1;
    const static int WAITING = 2;
    const static int STOPPING = -1;

    // return value > 0 indicates there is (are) event(s)
    // return value = 0 indicates there is still no event
    // return value < 0 indicates interrupted
    using Wait4Events = Callback<EventLoop *>;

    // return value is ignored
    using OnEvents = Callback<EventLoop *>;

    // run event loop and block current photon-thread, till the loop stopped
    virtual void run() = 0;
    // run event loop in new photon thread, so that will not block thread
    virtual void async_run() = 0;
    virtual void stop() = 0;

    int state() {
        return m_state;
    }

    photon::thread *loop_thread() {
        return m_thread;
    }

protected:
    EventLoop() {
    } // not allowed to directly construct
    photon::thread *m_thread = nullptr;
    int m_state = STOP;
};

extern "C" EventLoop *new_event_loop(EventLoop::Wait4Events wait, EventLoop::OnEvents on_event);
