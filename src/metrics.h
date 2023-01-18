#pragma once
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/thread/thread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <type_traits>

namespace Metric {

class ValueCounter {
public:
    int64_t counter = 0;

    void set(int64_t x) { counter = x; }
    int64_t val() { return counter; }
};

class AddCounter {
public:
    int64_t counter = 0;

    void inc() { counter++; }
    void dec() { counter--; }
    void add(int64_t x) { counter += x; }
    void sub(int64_t x) { counter -= x; }
    void reset() { counter = 0; }
    int64_t val() { return counter; }
};

class AverageCounter {
public:
    int64_t sum = 0;
    int64_t cnt = 0;
    uint64_t time = 0;
    uint64_t m_interval = 60UL * 1000 * 1000;

    void normalize() {
        auto now = photon::now;
        if (now - time > m_interval * 2) {
            reset();
        } else if (now - time > m_interval) {
            sum = photon::sat_sub(sum, sum * (now - time) / m_interval);
            cnt = photon::sat_sub(cnt, cnt * (now - time) / m_interval);
            time = now;
        }
    }
    void put(int64_t val, int64_t add_cnt = 1) {
        normalize();
        sum += val;
        cnt += add_cnt;
    }
    void reset() {
        sum = 0;
        cnt = 0;
        time = photon::now;
    }
    int64_t interval() { return m_interval; }
    int64_t interval(int64_t x) { return m_interval = x; }
    int64_t val() {
        normalize();
        return cnt ? sum / cnt : 0;
    }
};

class QPSCounter {
public:
    int64_t counter = 0;
    uint64_t time = photon::now;
    uint64_t m_interval = 1UL * 1000 * 1000;
    static constexpr uint64_t SEC = 1UL * 1000 * 1000;

    void normalize() {
        auto now = photon::now;
        if (now - time >= m_interval * 2) {
            reset();
        } else if (now - time > m_interval) {
            counter =
                photon::sat_sub(counter, counter * (now - time) / m_interval);
            time = now;
        }
    }
    void put(int64_t val = 1) {
        normalize();
        counter += val;
    }
    void reset() {
        counter = 0;
        time = photon::now;
    }
    uint64_t interval() { return m_interval; }
    uint64_t interval(uint64_t x) { return m_interval = x; }
    int64_t val() {
        normalize();
        return counter;
    }
};

class MaxCounter {
public:
    int64_t maxv = 0;

    void put(int64_t val) {
        if (val > maxv) {
            maxv = val;
        }
    }
    void reset() { maxv = 0; }
    int64_t val() { return maxv; }
};

class IntervalMaxCounter {
public:
    int64_t maxv = 0, last_max = 0;
    uint64_t time = 0;
    uint64_t m_interval = 5UL * 1000 * 1000;

    void normalize() {
        if (photon::now - time >= 2 * m_interval) {
            // no `val` or `put` call in 2 intervals
            // last interval max must become 0
            reset();
        } else if (photon::now - time > m_interval) {
            // one interval passed
            // current maxv become certainly max val in last interval
            last_max = maxv;
            maxv = 0;
            time = photon::now;
        }
    }

    void put(int64_t val) {
        normalize();
        maxv = val > maxv ? val : maxv;
    }

    void reset() {
        maxv = 0;
        last_max = 0;
        time = photon::now;
    }

    uint64_t interval() { return m_interval; }

    uint64_t interval(uint64_t x) { return m_interval = x; }

    int64_t val() {
        normalize();
        return maxv > last_max ? maxv : last_max;
    }
};

template <typename LatencyCounter>
class LatencyMetric {
public:
    LatencyCounter& counter;
    uint64_t start;

    explicit LatencyMetric(LatencyCounter& counter)
        : counter(counter), start(photon::now) {}

    // no copy or move;
    LatencyMetric(LatencyMetric&&) = delete;
    LatencyMetric(const LatencyMetric&) = delete;
    LatencyMetric& operator=(LatencyMetric&&) = delete;
    LatencyMetric& operator=(const LatencyMetric&) = delete;

    ~LatencyMetric() { counter.put(photon::now - start); }
};

class AverageLatencyCounter : public AverageCounter {
public:
    using MetricType = LatencyMetric<AverageLatencyCounter>;
};

class MaxLatencyCounter : public IntervalMaxCounter {
public:
    using MetricType = LatencyMetric<IntervalMaxCounter>;
};

#define SCOPE_LATENCY(x)                                                    \
    std::decay<decltype(x)>::type::MetricType _CONCAT(__audit_start_time__, \
                                                      __LINE__)(x);

static ALogLogger default_metrics_logger;

#define LOG_METRICS(...) (__LOG_METRICS__(ALOG_AUDIT, __VA_ARGS__))

#define __LOG_METRICS__(level, first, ...)                               \
    ({                                                                   \
        DEFINE_PROLOGUE(level, prolog);                                  \
        auto __build_lambda__ = [&](ILogOutput* __output_##__LINE__) {   \
            if (_IS_LITERAL_STRING(first)) {                             \
                __log__(level, __output_##__LINE__, prolog,              \
                        TSTRING(#first).template strip<'\"'>(),          \
                        ##__VA_ARGS__);                                  \
            } else {                                                     \
                __log__(level, __output_##__LINE__, prolog,              \
                        ConstString::TString<>(), first, ##__VA_ARGS__); \
            }                                                            \
        };                                                               \
        LogBuilder<decltype(__build_lambda__)>(                          \
            level, std::move(__build_lambda__),                          \
            &Metric::default_metrics_logger);                            \
    })

#define LOOP_APPEND_METRIC(ret, name)                                       \
    if (!va_##name.empty()) {                                               \
        ret.append(name.help_str()).append("\n");                           \
        ret.append(name.type_str()).append("\n");                           \
        for (auto x : va_##name) {                                          \
            ret.append(                                                     \
                   name.render(x.second->val(), nodename.c_str(), x.first)) \
                .append("\n");                                              \
        }                                                                   \
        ret.append("\n");                                                   \
    }

}  // namespace Metric
