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
#include "../thread.cpp"
#include "../thread11.h"
#include "../thread-pool.h"
#include <inttypes.h>
#include <math.h>
#include <string>
#include <random>
#include <queue>
#include <algorithm>
#include <sys/time.h>
#include <gtest/gtest.h>
#include <gflags/gflags.h>

using namespace std;
using namespace photon;

DEFINE_int32(ths_total, 0, "total threads when testing threadpool.");

semaphore aSem(4);
void* asdf(void* arg)
{
    LOG_DEBUG("Hello world, in photon thread-`! step 1", (uint64_t)arg);
    photon::thread_usleep(rand() % 1000 + 1000*500);
    LOG_DEBUG("Hello world, in photon thread-`! step 2", (uint64_t)arg);
    photon::thread_usleep(rand() % 1000 + 1000*500);
    LOG_DEBUG("Hello world, in photon thread-`! step 3", (uint64_t)arg);
    aSem.signal(1);
    return arg;
}

struct fake_context
{
    uint64_t a,b,c,d,e,f,g,h;
};

void fake_context_switch(fake_context* a, fake_context* b)
{
    a->a = b->a;
    a->b = b->b;
    a->c = b->c;
    a->d = b->d;
    a->e = b->e;
    a->f = b->f;
    a->g = b->g;
    a->h = b->h;
}

uint64_t isqrt(uint64_t n)
{
    if (n == 0)
        return 0;

    uint64_t x =  1L << (sizeof(n) * 8 / 2);
    while (true)
    {
        auto y = (x + n / x) / 2;
        if (y >= x)
            return x;
        x = y;
    }
}

inline uint64_t now_time()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 * 1000 + tv.tv_usec;
}

template<typename B> static inline __attribute__((always_inline))
void do_benchmark(const char* title, B callback)
{
    uint64_t n, N = 0;
    uint64_t T = now_time();
    while(true)
    {
        for (n=0; n<1000*1000; ++n)
        {
            callback();
        }
        N += n;
        uint64_t t = now_time() - T;
        if (t > 5*1000*1000) { T = t; break; }
    }
    printf("time of %16s: %'0.2f ns / iteration, or %'lu iterations / s, on average of %'lu M iterations\n",
           title, (double)(T * 1000) / N, N * 1000 * 1000 / T, N / 1000 / 1000);

}

void test_isqrt(uint64_t x)
{
    LOG_DEBUG("isqrt(`)=`, sqrt(`)=`", x, isqrt(x), x, sqrt(x));
}

void test_isqrt_all()
{
    double maxd = 0;
    LOG_DEBUG("RAND_MAX=", RAND_MAX);
    for (uint64_t i = 0; i < 100000000; ++i)
    {
        uint64_t x = rand();
        auto d = sqrt(x) - isqrt(x);
        maxd = max(d, maxd);
        if (d > 2)
        {
            test_isqrt(x);
            return;
        }
    }
    LOG_DEBUG("maxd=", maxd);
}

int DevNull(void*, int);
void sqrt_benchmark()
{
    do_benchmark("isqrt", [&]() __attribute__((always_inline)) {
        auto x = isqrt(rand());
        DevNull(&x, sizeof(x));
    });
    do_benchmark(" sqrt", [&]() __attribute__((always_inline)) {
        auto x = sqrt(rand());
        DevNull(&x, sizeof(x));
    });
    do_benchmark("loop", [&]() __attribute__((always_inline)) {
        auto x = rand();
        DevNull(&x, sizeof(x));
    });
}

__attribute__((noinline))
void test_sat_add()
{
    uint64_t a = -1, b = 10;
    auto c = sat_add(a, b);
    LOG_DEBUG(c);
}

bool check()
{
    for (size_t i = 0; (i << 1) + 1 < sleepq.q.size(); i++) {
        size_t l = (i << 1) + 1;
        size_t r = l + 1;
        assert(sleepq.q[i]->ts_wakeup <= sleepq.q[l]->ts_wakeup);
        if (r < sleepq.q.size())
            assert(sleepq.q[i]->ts_wakeup <= sleepq.q[r]->ts_wakeup);
    }
    return true;
}

void print_heap()
{
    LOG_INFO("    ");
    int i = 0, k = 1;
    for (auto it : sleepq.q) {
        printf("%lu(%d)", it->ts_wakeup, it->idx);
        i++;
        if (i == k) {
            printf("\n");
            i = 0;
            k <<= 1;
        } else printf(" ");
    }
    printf("\n");
}

TEST(Sleep, queue)
{
    static int heap_size  = 1000000;
    static int rand_limit = 100000000;
    log_output_level = 1; // 0 will print heap structure.
    DEFER(log_output_level = 0);

    struct timeval t0, t1;
    auto time_elapsed = [&](){
        return (t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_usec - t0.tv_usec);
    };
    auto seed = /* time(0); */10007;
    srand(seed);
    sleepq.q.reserve(heap_size);
    vector<thread *> items{};

    auto perf = [&](){
        LOG_INFO("============ performance test ==============");
        LOG_INFO("BUILD HEAP (push) `K items into sleepq.", items.size() / 1000);
        gettimeofday(&t0, nullptr);
        for (auto &it : items){
            sleepq.push(it);
        }
        gettimeofday(&t1, nullptr);
        LOG_INFO("time elapsed: ` us", time_elapsed());
        LOG_INFO("--------------------------------------------");
        if (log_output_level == 0) print_heap();
        check();
        auto pops = items;
        random_shuffle(pops.begin(), pops.end());
        pops.erase(pops.begin() + heap_size / 2, pops.end() );
        LOG_INFO("RANDOM POP `K items in sleepq", pops.size() / 1000);
        gettimeofday(&t0, nullptr);
        for (auto& it : pops){
            sleepq.pop(it);
        };
        gettimeofday(&t1, nullptr);
        LOG_INFO("time elapsed: ` us", time_elapsed());
        LOG_INFO("--------------------------------------------");
        if (log_output_level == 0) print_heap();
        check();
        while (sleepq.empty() == false){
            auto ret = sleepq.pop_front();
            _unused(ret);
        }
        LOG_INFO("REBUILD HEAP");
        for (auto& it : items){
            sleepq.push(it);
        }
        LOG_INFO("--------------------------------------------");
        LOG_INFO("POP FRONT `K items.", sleepq.q.size() / 1000);
        gettimeofday(&t0, nullptr);
        while (sleepq.empty() == false){
           auto ret = sleepq.pop_front();
           _unused(ret);
        }
        gettimeofday(&t1, nullptr);
        LOG_INFO("time elapsed: ` us", time_elapsed());
        LOG_INFO("============================================");
    };
    LOG_INFO("ITEMS WITH SAME VALUE.");
    for (int i = 0; i < heap_size; i++){
        auto th = new thread();
        th->ts_wakeup = 1000;/* rand() % 100000000 */;
        items.emplace_back(th);
    }
    perf();

    LOG_INFO("ITEMS WITH RANDOM VALUE.");
    items.clear();
    for (int i = 0; i < heap_size; i++){
        auto th = new thread();
        th->ts_wakeup = rand() % rand_limit;
        items.emplace_back(th);
    }
    perf();

    LOG_INFO("sleepq test done.");
    for (auto it : items)
        delete it;
}

photon::condition_variable aConditionVariable;
photon::mutex aMutex;

void* thread_test_function(void* arg)
{
    LOG_DEBUG(VALUE(CURRENT), " before lock");
    scoped_lock aLock(aMutex);
    LOG_DEBUG(VALUE(CURRENT), " after lock");

    uint32_t* result = (uint32_t*)arg;

    *result += 100;
//    for(uint32_t i = 0; i < 100; ++i)
//    {
//        (*result)++;
//    }

    LOG_DEBUG(VALUE(CURRENT), " before sleep");
    photon::thread_usleep(100);
    LOG_DEBUG(VALUE(CURRENT), " after sleep");

    *result += 100;
//    for(uint32_t i = 0; i < 100; ++i)
//    {
//        (*result)++;
//    }

    LOG_DEBUG(VALUE(CURRENT), " before yield");
    photon::thread_yield_to(nullptr);
    LOG_DEBUG(VALUE(CURRENT), " after yield");

    *result += 100;
//    for(uint32_t i = 0; i < 100; ++i)
//    {
//        (*result)++;
//    }

    aConditionVariable.notify_one();
    aConditionVariable.notify_all();

    LOG_DEBUG(VALUE(CURRENT), " after notified, about to release lock and exit");
    return arg;
}

TEST(ThreadTest, HandleNoneZeroInput)
{
    uint32_t result = 0;
    thread* th1 = photon::thread_create(&thread_test_function, (void*)&result);
    while (true)
    {
        photon::thread_usleep(0);
        CURRENT->ts_wakeup = sat_add(now, 10000);
        if (CURRENT->single())
        {
            if (sleepq.empty()) break;
            else ::usleep(sleepq.front()->ts_wakeup - now);
        }
    }
    _unused(th1);
    // EXPECT_EQ(photon::states::DONE, thread_stat(th1));
    photon::thread_yield();
    photon::thread_yield_to(nullptr);
    photon::thread_usleep(0);
    photon::thread_usleep(1000);

    thread* th2 = photon::thread_create(&thread_test_function, (void*)&result);
    thread* th3 = photon::thread_create(&thread_test_function, (void*)&result);
    _unused(th2);
    _unused(th3);

    LOG_DEBUG("before aConditionVariable.wait_no_lock");
    EXPECT_EQ(0, aConditionVariable.wait_no_lock(1000*1000*1000));
    LOG_DEBUG("after aConditionVariable.wait_no_lock");

//    thread_interrupt(th2,0);

    while (true)
    {
        photon::thread_usleep(0);
        CURRENT->ts_wakeup = sat_add(now, 10000);
        if (CURRENT->single())
        {
            if (sleepq.empty()) break;
            else ::usleep(sleepq.front()->ts_wakeup - now);
        }
    }

    EXPECT_EQ(900U, result);

    set_idle_sleeper(get_idle_sleeper());
}

TEST(ListTest, HandleNoneZeroInput)
{
    struct listElement :
        public intrusive_list_node<listElement> { };

    listElement* aList = nullptr;
    for (int i = 0; i < 10; ++i)
    {
        listElement* newElement = new listElement();
        if (!aList)
        {
            aList = newElement;
        }
        else
        {
            aList->insert_tail(newElement);
        }
    }

    listElement::iterator it = aList->begin();

    int deleteCount = 0;
    std::vector<listElement*> delete_vector;

    for ( ;it != aList->end(); ++it)
    {
        ++deleteCount;
        delete_vector.push_back(&(*it));
    }

    for (auto e : delete_vector)
    {
        delete e;
    }

    EXPECT_EQ(10, deleteCount);
}

static int running = 0;
void* thread_pong(void* depth)
{
#ifdef RANDOMIZE_SP
    if (depth)
        return thread_pong((void*)((uint64_t)depth - 1));
#endif
    running = 1;
    while (running)
        thread_yield_to(nullptr);
    return nullptr;
}

void test_thread_switch(uint64_t nth, uint64_t stack_size)
{
    const uint64_t total = 100*1000*1000;
    uint64_t count = total / nth;

    for (uint64_t i = 0; i < nth - 1; ++i)
        photon::thread_create(&thread_pong, (void*)(uint64_t)(rand() % 32), stack_size);

    for (uint64_t i = 0; i < count; ++i)
        thread_yield_to(nullptr);

    auto t0 = now_time();
    for (uint64_t i = 0; i < count; ++i)
        thread_yield_to(nullptr);

    auto t1 = now_time();

    for (uint64_t i = 0; i < count; ++i)
        DevNull(&i, 0);

    auto t2 = now_time();

    auto d1 = t1 - t0;
    auto d2 = (t2 - t1) * nth;
    LOG_INFO("threads `, stack-size `, time `, loop time `, ", nth, stack_size, d1, d2);
    LOG_INFO("single context switch: ` ns (` M/s)",
             double((d1 - d2)*1000) / total, total / double(d1 - d2));

    running = 0;
    while(!CURRENT->single())
        thread_yield_to(nullptr);
}

TEST(Perf, ThreadSwitch)
{
    test_thread_switch(100, 8 * 1024 * 1024);
    return;

    const uint64_t stack_size = 8 * 1024 * 1024;
    LOG_INFO(VALUE(stack_size));
    test_thread_switch(2, stack_size);
    test_thread_switch(10, stack_size);
    test_thread_switch(100, stack_size);
    test_thread_switch(1000, stack_size);
    test_thread_switch(10000, stack_size);
    test_thread_switch(100000, stack_size);

    for (uint64_t ss = 5; ss <= 13; ++ss)
    {
        test_thread_switch(100000, (1<<ss) * 1024);
    }
}

int shot_count = 0;
photon::condition_variable shot_cond;
uint64_t on_timer_asdf(void* arg)
{
    shot_count++;
    LOG_DEBUG(VALUE(arg), VALUE(shot_count));
    shot_cond.notify_one();
    return 0;
}

void wait_for_shot()
{
    LOG_DEBUG("wait for a timer to occur ", VALUE(now));
    auto t0 = now;
    shot_cond.wait_no_lock();
    auto delta_time = now - t0;
    LOG_DEBUG("ok, the timer fired ", VALUE(now), VALUE(delta_time));
}

TEST(Timer, OneShot)
{
    // since last test is perf, will not update photon::now
    // all timers may trigger before wait_for_shot() called.
    // photon::now should be update.
    photon::thread_yield();
    Timer timer1(-1, {&on_timer_asdf, &shot_cond}, true);
    timer1.reset(1000*1000);
    Timer timer2(-1, {&shot_cond, &on_timer_asdf}, false);
    timer2.reset(500*1000);
    wait_for_shot();
    wait_for_shot();
    EXPECT_EQ(shot_count, 2);
}

void test_reuse(Timer& timer)
{
    timer.reset(100*1000);
    wait_for_shot();
    timer.reset(300*1000);
    wait_for_shot();
}

TEST(Timer, Reuse)
{
    {
        Timer timer(1000*1000, {&on_timer_asdf, &shot_count});
        wait_for_shot();
        test_reuse(timer);
    }
    {
        Timer timer(-1, {&on_timer_asdf, nullptr});
        test_reuse(timer);
    }
}

uint64_t t0;
int timer_count = 5;
uint64_t on_timer(void* arg)
{
    EXPECT_EQ(arg, &timer_count);
    auto t1 = now_time();
    auto delta_t = t1 - t0;
    t0 = t1;
    LOG_INFO(VALUE(delta_t));
    LOG_INFO(VALUE(timer_count));
    if (timer_count == 0)
    {
        timer_count = -1;
        return -1;
    }
    return timer_count-- * (100*1000);
}

TEST(Timer, Reapting)
{
    t0 = now_time();
    Timer timer(1000*1000, {&on_timer, &timer_count});
    // auto timer = timer_create(1000*1000, &on_timer, &timer_count, TIMER_FLAG_REPEATING);
    // _unused(timer);
    while(timer_count >= 0)
        thread_usleep(1000);
}

struct st_arg {
    photon::condition_variable start;
    int count = 0;
};
uint64_t sleep_timer(void* arg) {
    auto q = (st_arg*)arg;
    LOG_DEBUG("FIRE");
    q->count ++;
    q->start.notify_one();
    photon::thread_usleep(500*1000);
    return 0;
}

TEST(Timer, StopWhileFiring) {
    st_arg a;
    Timer timer(1000*1000, {&sleep_timer, &a});
    a.start.wait_no_lock();
    LOG_DEBUG("CALL STOP");
    // now timer is not waiting, try stop
    timer.stop();
    LOG_DEBUG("STOPPED");
    // then it will not continue running;
    thread_usleep(2000*1000);
    // make sure it will not fire again
    EXPECT_EQ(1, a.count);
}

vector<int> aop;

TEST(Thread, function)
{
    photon::join_handle* th1 = photon::thread_enable_join(photon::thread_create(&asdf, (void*)0));
    photon::join_handle* th2 = photon::thread_enable_join(photon::thread_create(&asdf, (void*)1));
    photon::join_handle* th3 = photon::thread_enable_join(photon::thread_create(&asdf, (void*)2));

    photon::thread_yield();
    photon::thread_usleep(0);
    photon::thread_usleep(1000);

    LOG_DEBUG("before join");
    photon::thread_join(th1);
    photon::thread_join(th2);
    photon::thread_join(th3);
    LOG_DEBUG("after join");
}

TEST(thread11, example)
{
    __Example_of_Thread11__ example;
    example.asdf();
}

class Marker : public string
{
public:
    using string::string;
    Marker(const Marker& rhs) :
        string(rhs)
    {
        auto r = (void*)&rhs;
        LOG_DEBUG(VALUE(this), "(&)", VALUE(r));
    }
    Marker(Marker&& rhs) :
        string(move(rhs))
    {
        auto r = (void*)&rhs;
        LOG_DEBUG(VALUE(this), "(&&)", VALUE(r));
    }
    void operator = (const Marker& rhs)
    {
        auto r = (void*)&rhs;
        LOG_DEBUG(VALUE(this), "=(&)", VALUE(r));
        string::operator=(rhs);
    }
    void operator = (Marker&& rhs)
    {
        auto r = (void*)&rhs;
        LOG_DEBUG(VALUE(this), "=(&&)", VALUE(r));
        string::operator=(rhs);
    }

    void randwrite(void* file, size_t nwrites);

//    ~Marker()                                   { LOG_DEBUG(VALUE(this), "~()"); }
};

Marker a("asdf"), b("bbsitter"), c("const");
Marker aa(a.c_str()), cc(c.c_str());

void Marker::randwrite(void* file, size_t nwrites)
{
    EXPECT_EQ(this, &c);
    EXPECT_EQ(file, (void*)1234567);
    EXPECT_EQ(1244UL, nwrites);
}

int test_thread(Marker x, Marker& y, Marker z, int n)
{
    LOG_DEBUG(' ', n);
    EXPECT_EQ(x, a);
    EXPECT_EQ(&y, &b);
    EXPECT_EQ(z, cc);
    EXPECT_EQ(c, "");
    return 0;
}

int test_thread2(Marker x, Marker& y, Marker&& z, int n)
{
    LOG_DEBUG(' ', n);
    EXPECT_EQ(x, aa);
    EXPECT_EQ(a, "");
    EXPECT_EQ(&y, &b);
    EXPECT_EQ(&z, &c);
    EXPECT_EQ(c, cc);
    return 0;
}

TEST(thread11, test)
{
    LOG_DEBUG(VALUE(&a), VALUE(&b), VALUE(&c));
    {
        LOG_DEBUG(' ');
        test_thread(a, b, std::move(c), 30);
        a.assign(aa); c.assign(cc);
    }
    {
        LOG_DEBUG(' ');
        auto th = thread_create11(&test_thread, a, b, std::move(c), 31);
        auto jh = thread_enable_join(th);
        thread_join(jh);
        a.assign(aa); c.assign(cc);
    }

    {
        int n = 42;
        LOG_DEBUG(' ');
        test_thread2(std::move(a), (b), std::move(c), std::move(n));
        a.assign(aa); c.assign(cc);
    }
    {
        int n = 43;
        LOG_DEBUG(' ');
        auto th = thread_create11(&test_thread2, std::move(a), (b), std::move(c), std::move(n));
        auto jh = thread_enable_join(th);
        thread_join(jh);
        a.assign(aa); c.assign(cc);
    }

    {
        LOG_DEBUG(' ');
        auto th = thread_create11(&Marker::randwrite, &c, (void*)1234567, 1244);
        auto jh = thread_enable_join(th);
        thread_join(jh);
    }
    {
        // int n = 11;
        auto th = thread_create11(&Marker::randwrite, &c, (void*)1234567, 1244);
        auto jh = thread_enable_join(th);
        thread_join(jh);
    }
}

vector<uint64_t> issue_list;
queue<uint64_t> processing_queue;


void semaphore_test_hold(semaphore* sem, int &step) {
    int ret = 0;
    step++;
    EXPECT_EQ(1, step);
    sem->signal(1);
    LOG_DEBUG("+1");
    ret = sem->wait(2);
    EXPECT_EQ(0, ret);
    LOG_DEBUG("-2");
    step++;
    EXPECT_EQ(3, step);
    sem->signal(3);
    LOG_DEBUG("+3");
    ret = sem->wait(4);
    EXPECT_EQ(0, ret);
    step++;
    EXPECT_EQ(5, step);
    LOG_DEBUG("-4");
}

void semaphore_test_catch(semaphore* sem, int &step) {
    int ret = 0;
    ret = sem->wait(1);
    EXPECT_EQ(0, ret);
    LOG_DEBUG("-1");
    step++;
    EXPECT_EQ(2, step);
    sem->signal(2);
    LOG_DEBUG("+2");
    ret = sem->wait(3);
    EXPECT_EQ(0, ret);
    LOG_DEBUG("-3");
    step++;
    EXPECT_EQ(4, step);
    sem->signal(4);
    LOG_DEBUG("+4");
}

TEST(Semaphore, basic) {
    int step = 0;
    semaphore sem(0);
    auto th1 = thread_create11(semaphore_test_catch, &sem, step);
    auto th2 = thread_create11(semaphore_test_hold, &sem, step);
    auto jh1 = thread_enable_join(th1);
    auto jh2 = thread_enable_join(th2);
    thread_join(jh1);
    thread_join(jh2);
}

int cnt = 0;
void semaphore_heavy(semaphore& sem, int tid) {
//    LOG_DEBUG("enter");
    cnt++;
    sem.wait(tid);
    sem.signal(tid + 1);
    cnt--;
}

TEST(Semaphore, heavy) {
    semaphore sem(0);
    const int thread_num = 100000;
    for (int i=1; i<=thread_num;i++) {
        thread_create11(semaphore_heavy, sem, i);
    }
    LOG_DEBUG("created ` threads", thread_num);
    sem.signal(1);
    auto ret = sem.wait(thread_num+2, 1);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ETIMEDOUT, errno);
    ret = sem.wait(thread_num+1);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0UL, sem.m_count);
    while (!(CURRENT->single())) {thread_yield_to(nullptr);}
}

TEST(Sleep, sleep_only_thread) {
    // If current thread is only thread and sleeping
    // it should be able to avoid crash during long time sleep
    auto start = photon::now;
    photon::thread_sleep(5);
    EXPECT_GT(photon::now - start, 4UL*1024*1024);
}

void *func1(void *)
{
    photon::thread_sleep(rand()%5);
    LOG_INFO("hello");
    return nullptr;
}

TEST(ThreadPool, test)
{
    ThreadPool<512> pool;
    TPControl *ths[512];
    for (int i = 0; i<FLAGS_ths_total; i++)
        ths[i] = pool.thread_create_ex(&::func1, nullptr, true);
    LOG_INFO("----------");
    for (int i = 0; i<FLAGS_ths_total; i++) {
        LOG_DEBUG("wait thread: `", ths[i]->th);
        pool.join(ths[i]);
    }
    // pool.wait_all();
    LOG_INFO("???????????????");
}

uint64_t rw_count;
bool writing = false;
photon::rwlock rwl;

void *rwlocktest(void* args) {
    uint64_t carg = (uint64_t) args;
    auto mode = carg & ((1UL<<32) -1);
    auto id = carg >> 32;
    // LOG_DEBUG("locking ", VALUE(id), VALUE(mode));
    rwl.lock(mode);
    LOG_DEBUG("locked ", VALUE(id), VALUE(mode));
    EXPECT_EQ(id, rw_count);
    rw_count ++;
    if (mode == photon::RLOCK)
        EXPECT_FALSE(writing);
    else
        writing = true;
    photon::thread_usleep(100*1000);
    if (mode == photon::WLOCK)
        writing = false;
    LOG_DEBUG("unlocking ", VALUE(id), VALUE(mode));
    rwl.unlock();
    // LOG_DEBUG("unlocked ", VALUE(id), VALUE(mode));
    return NULL;
}

TEST(RWLock, checklock) {
    std::vector<photon::join_handle*> handles;
    rw_count = 0;
    writing = false;
    for (uint64_t i=0; i<100;i++) {
        uint64_t arg = (i << 32) | (rand()%10 < 7 ? photon::RLOCK : photon::WLOCK);
        handles.emplace_back(
            photon::thread_enable_join(
                photon::thread_create(&rwlocktest, (void*)arg)
            )
        );
    }
    for (auto &x : handles)
        photon::thread_join(x);
    EXPECT_EQ(100UL, rw_count);
}

void* interrupt(void* th) {
    photon::thread_interrupt((photon::thread*)th, EALREADY);
    return 0;
}

TEST(RWLock, interrupt) {
    rwlock rwl;
    int ret = rwl.lock(photon::WLOCK); // write lock
    EXPECT_EQ(0, ret);
    ret = rwl.lock(photon::RLOCK, 1000UL); // it should not be locked
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ETIMEDOUT, errno);
    photon::thread_create(&interrupt, photon::CURRENT);
    ret = rwl.lock(photon::RLOCK);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(EALREADY, errno);
}

int main(int argc, char** arg)
{
    photon::init();
    ::testing::InitGoogleTest(&argc, arg);
    google::ParseCommandLineFlags(&argc, &arg, true);

    LOG_DEBUG("test result:`",RUN_ALL_TESTS());
    return 0;
    test_sat_add();
//    test_sleepqueue();

    srand(time(0));

    //aSem.wait(1);
    aSem.wait(3);
    //aSem.wait(10);

    while (true)
    {
        photon::thread_usleep(0);
        if (CURRENT->single())
        {
            if (sleepq.empty()) break;
            else ::usleep(sleepq.front()->ts_wakeup - now);
        }
    }
}

int DevNull(void* x, int)
{
    return 0;
}

int (*pDevNull)(void*, int) = &DevNull;
