/*
 * identity-pool.cpp
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
#include "identity-pool.h"
#include "photon/timer.h"

struct ScalePoolController;
static ScalePoolController *g_scale_pool_controller;
struct ScalePoolController {
    photon::Timer timer;
    intrusive_list<IdentityPoolBase> entries;
    ScalePoolController(uint64_t interval = 1000UL * 1000)
        : timer(interval, {this, &ScalePoolController::scan_pool_scale}) {
    }

    ~ScalePoolController() {
    }
    photon::mutex mutex;

    uint64_t scan_pool_scale() {
        photon::scoped_lock lock(mutex);
        for (auto &e : entries) {
            e.do_scale();
        }
        return 0;
    }

    int register_pool(IdentityPoolBase *entry) {
        photon::scoped_lock lock(mutex);
        entries.push_back(entry);
        return 0;
    }

    int unregister_pool(IdentityPoolBase *entry) {
        photon::scoped_lock lock(mutex);
        entries.pop(entry);
        return 0;
    }
};

int IdentityPoolBase::enable_autoscale() {
    if (autoscale)
        return -1;
    if (g_scale_pool_controller == nullptr) {
        g_scale_pool_controller = new ScalePoolController();
    }
    g_scale_pool_controller->register_pool(this);
    autoscale = true;
    return 0;
}

int IdentityPoolBase::disable_autoscale() {
    if (!autoscale)
        return -1;
    g_scale_pool_controller->unregister_pool(this);
    autoscale = false;
    if (g_scale_pool_controller->entries.empty()) {
        auto ctl = g_scale_pool_controller;
        g_scale_pool_controller = nullptr;
        delete ctl;
    }
    return 0;
}
