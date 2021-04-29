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
#include <curl/curl.h>
#include <string>

#include "../alog.h"
#include "../event-loop.h"
#include "../photon/syncio/fd-events.h"
#include "../photon/thread11.h"
#include "../photon/timer.h"
#include "../timeout.h"
#include "../utility.h"

namespace Net {
static constexpr int poll_size = 16;
static photon::Timer *g_timer;
static CURLM *g_libcurl_multi;
static photon::FD_Poller *g_poller;
static CURLcode global_initialized;
struct async_libcurl_operation {
    photon::condition_variable cv;
    photon::thread *th = nullptr;
};
static int do_action(curl_socket_t fd, int event) {
    int running_handles;
    auto ret = curl_multi_socket_action(g_libcurl_multi, fd, event, &running_handles);
    if (ret != CURLM_OK)
        LOG_ERROR_RETURN(EIO, -1,
                         "failed to curl_multi_socket_action(): ", curl_multi_strerror(ret));
    int msgs_left;
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(g_libcurl_multi, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            auto fcurl = msg->easy_handle;
            async_libcurl_operation *faop;
            char *eff_url;
            curl_easy_getinfo(fcurl, CURLINFO_EFFECTIVE_URL, &eff_url);
            auto res = curl_easy_strerror(msg->data.result);
            LOG_DEBUG("DONE: ` => (`) ", eff_url, res);
            CURLcode ret = curl_easy_getinfo(fcurl, CURLINFO_PRIVATE, &faop);
            if (ret == CURLE_OK && faop != nullptr) {
                LOG_DEBUG(VALUE(fcurl), " FINISHED");
                faop->cv.notify_one();
                photon::thread_yield_to(faop->th);
            }
        }
    }
    return 0;
}
int curl_perform(CURL *curl, uint64_t timeout) {
    Timeout tmo(timeout);
    async_libcurl_operation aop;
    aop.th = photon::CURRENT;
    CURLcode ret = curl_easy_setopt(curl, CURLOPT_PRIVATE, &aop);
    if (ret != CURLE_OK)
        LOG_ERROR_RETURN(ENXIO, ret, "failed to set libcurl private: ", curl_easy_strerror(ret));
    DEFER(curl_easy_setopt(curl, CURLOPT_PRIVATE, nullptr));
    // this will cause set timeout
    CURLMcode mret = curl_multi_add_handle(g_libcurl_multi, curl);
    if (mret != CURLM_OK)
        LOG_ERROR_RETURN(EIO, mret,
                         "failed to curl_multi_add_handle(): ", curl_multi_strerror(mret));
    DEFER(curl_multi_remove_handle(g_libcurl_multi, curl));
    // perform start
    int wait = aop.cv.wait_no_lock(tmo.timeout());
    if (wait < 0)
        LOG_ERROR_RETURN(0, CURLM_INTERNAL_ERROR, "failed to cvar.wait for event");
    LOG_DEBUG("FINISHED");
    return CURLM_OK;
}
static uint64_t on_timer(void * = nullptr) {
    photon::thread_create11(&do_action, CURL_SOCKET_TIMEOUT, 0);
    return 0;
}
/* CURLMOPT_TIMERFUNCTION */
static int timer_cb(CURLM *, long timeout_ms, void *) {
    if (timeout_ms >= 0) {
        g_timer->reset(timeout_ms * 1000UL);
    }
    return 0;
}

/* CURLMOPT_SOCKETFUNCTION */
static int sock_cb(CURL *curl, curl_socket_t fd, int event, void *, void *) {
    async_libcurl_operation *aop;
    CURLcode ret = curl_easy_getinfo(curl, CURLINFO_PRIVATE, &aop);
    if (ret != CURLE_OK || aop == nullptr)
        LOG_ERROR_RETURN(EINVAL, -1, "failed to get CURLINFO_PRIVATE from CURL* ", curl);
    uint32_t ev = 0;
    if (event & CURL_POLL_REMOVE) {
        photon::fd_interest(g_poller, {fd, 0}, nullptr);
    }
    if (event & CURL_POLL_IN) {
        ev |= photon::EVENT_READ;
    }
    if (event & CURL_POLL_OUT) {
        ev |= photon::EVENT_WRITE;
    }
    if (ev != 0 && fd != CURL_SOCKET_BAD) {
        uint64_t data = (fd << 2) | ev;
        fd_interest(g_poller, {fd, ev}, (void *)data);
    }
    return 0;
}

class cURLLoop : public Object {
public:
    cURLLoop() : loop(new_event_loop({this, &cURLLoop::wait_fds}, {this, &cURLLoop::on_poll})) {
    }

    ~cURLLoop() override {
        delete loop;
    }

    void start() {
        loop->async_run();
    }

    void stop() {
        loop->stop();
    }

protected:
    EventLoop *loop;
    int cnt;
    uint64_t cbs[poll_size];

    int wait_fds(EventLoop *) {
        cnt = photon::wait_for_fds(g_poller, (void **)&cbs, poll_size);
        if (cnt > 0)
            return 1;
        if (errno == EINTR)
            return -1;
        return 0;
    }

    int on_poll(EventLoop *) {
        for (int i = 0; i < cnt; i++) {
            int fd = cbs[i] >> 2;
            int ev = cbs[i] & 0b11;
            if (fd != CURL_SOCKET_BAD && ev != 0)
                do_action(fd, ev);
        }
        return 0;
    }
};

static cURLLoop g_loop;

// CAUTION: this feature is incomplete in curl
int libcurl_set_pipelining(long val) {
    return curl_multi_setopt(g_libcurl_multi, CURLMOPT_PIPELINING, val);
}
// this feature seems not able to use in 7.29.0
int libcurl_set_maxconnects(long val) {
    // return curl_multi_setopt(g_libcurl_multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
    //                          val);
    return 0;
}
__attribute__((constructor)) void global_init() {
    global_initialized = curl_global_init(CURL_GLOBAL_ALL);
}

__attribute__((destructor)) void global_fini() {
    curl_global_cleanup();
}
int libcurl_init(long flags, long pipelining, long maxconn) {
    g_poller = photon::new_fd_poller(nullptr);
    g_loop.start();
    g_timer = new photon::Timer(-1UL, {nullptr, &on_timer});
    if (!g_timer)
        LOG_ERROR_RETURN(EFAULT, -1, "failed to create photon timer");

    if (global_initialized != CURLE_OK)
        LOG_ERROR_RETURN(EIO, -1,
                         "CURL global init error: ", curl_easy_strerror(global_initialized));

    LOG_DEBUG("libcurl version ", curl_version());

    g_libcurl_multi = curl_multi_init();
    if (g_libcurl_multi == nullptr)
        LOG_ERROR_RETURN(EIO, -1, "failed to init libcurl-multi");

    curl_multi_setopt(g_libcurl_multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
    curl_multi_setopt(g_libcurl_multi, CURLMOPT_TIMERFUNCTION, timer_cb);

    libcurl_set_pipelining(pipelining);
    libcurl_set_maxconnects(maxconn);

    return 0;
}
void libcurl_fini() {
    g_loop.stop();
    CURLMcode ret = curl_multi_cleanup(g_libcurl_multi);
    if (ret != CURLM_OK)
        LOG_ERROR("libcurl-multi cleanup error: ", curl_multi_strerror(ret));

    if (g_timer) {
        delete g_timer;
    }
}

std::string url_escape(const char *str) {
    auto s = curl_escape(str, 0);
    DEFER(curl_free(s));
    return std::string(s);
}
} // namespace Net
