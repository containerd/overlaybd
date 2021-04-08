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
