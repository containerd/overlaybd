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
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <utility>
#include <type_traits>
#include "utility.h"
#include "object.h"

class ILogOutput : public Object {
public:
    virtual void write(int level, const char *begin, const char *end) = 0;
    virtual int get_log_file_fd() = 0;
    virtual uint64_t set_throttle(uint64_t t = -1UL) = 0;
    virtual uint64_t get_throttle() = 0;
};

extern ILogOutput *const log_output_null;
extern ILogOutput *const log_output_stderr;
extern ILogOutput *const log_output_stdout;

ILogOutput *new_log_output_file(const char *fn, uint64_t rotate_limit = UINT64_MAX,
                                int max_log_files = 10, uint64_t throttle = -1UL);
ILogOutput *new_log_output_file(int fd, uint64_t rotate_limit = UINT64_MAX,
                                uint64_t throttle = -1UL);

// old-style log_output_file & log_output_file_close
// return 0 when successed, -1 for failed
int log_output_file(int fd, uint64_t rotate_limit = UINT64_MAX, uint64_t throttle = -1UL);
int log_output_file(const char *fn, uint64_t rotate_limit = UINT64_MAX, int max_log_files = 10,
                    uint64_t throttle = -1UL);
int log_output_file_close();

#ifndef LOG_BUFFER_SIZE
// size of a temp buffer on stack to format a log, deallocated after output
#define LOG_BUFFER_SIZE 4096
#endif

// wrapper for an integer, together with format information
struct ALogInteger {
public:
    template <typename T, ENABLE_IF(std::is_integral<T>::value)>
    ALogInteger(T x, char shift) {
        _signed = std::is_signed<T>::value;
        if (_signed)
            _svalue = x;
        else
            _uvalue = x;
        _shift = shift;
    }
    uint64_t uvalue() const {
        return _uvalue;
    }
    int64_t svalue() const {
        return _svalue;
    }
    bool is_signed() const {
        return _signed;
    }
    char shift() const {
        return _shift;
    }
    char width() const {
        return _width;
    }
    char padding() const {
        return _padding;
    }
    bool comma() const {
        return _comma;
    }
    bool lower() const {
        return _lower;
    }
    ALogInteger &width(char x) {
        this->_width = x;
        return *this;
    }
    ALogInteger &padding(char x) {
        this->_padding = x;
        return *this;
    }
    ALogInteger &comma(bool x) {
        this->_comma = x;
        return *this;
    }
    ALogInteger &lower(bool x) {
        this->_lower = x;
        return *this;
    }

protected:
    union {
        uint64_t _uvalue;
        int64_t _svalue;
    };
    char _shift;
    char _width = 0;
    char _padding = ' ';
    char _comma = false;
    bool _lower = false;
    bool _signed;
};

template <typename T>
ALogInteger HEX(T x) {
    return ALogInteger(x, 4).padding('0');
}

template <typename T>
ALogInteger DEC(T x) {
    return ALogInteger(x, 10);
}

template <typename T>
ALogInteger OCT(T x) {
    return ALogInteger(x, 3);
}

template <typename T>
ALogInteger BIN(T x) {
    return ALogInteger(x, 1);
}

// wrapper for an floating point value, together with format information
struct FP {
public:
    explicit FP(double x) : _value(x) {
    }
    double value() const {
        return _value;
    }
    char width() const {
        return _width;
    }
    char padding() const {
        return _padding;
    }
    char precision() const {
        return _precision;
    }
    bool comma() const {
        return _comma;
    }
    bool lower() const {
        return _lower;
    }
    bool scientific() const {
        return _scientific;
    }
    FP &width(char x) {
        _width = x;
        return *this;
    }
    FP &width(char x, char y) {
        _width = x;
        _precision = y;
        return *this;
    }
    FP &precision(char x, char y) {
        return width(x, y);
    }
    FP &precision(char x) {
        _precision = x;
        return *this;
    }
    FP &padding(char x) {
        _padding = x;
        return *this;
    }
    FP &comma(bool x) {
        _comma = x;
        return *this;
    }
    FP &lower(bool x) {
        _lower = x;
        return *this;
    }
    FP &scientific(bool x) {
        _scientific = x;
        return *this;
    }

protected:
    double _value;
    char _width = -1;
    char _precision = -1;
    char _padding = '0';
    bool _comma = false;
    bool _lower = false;
    bool _scientific = false;
};

struct ALogString {
    const char *const s;
    size_t size;

    constexpr ALogString(const char *s, size_t n) : s(s), size(n) {
    }

    constexpr char operator[](size_t i) const {
        return s[i];
    }
};

struct ALogStringL : public ALogString {
    using ALogString::ALogString;

    template <size_t N>
    constexpr ALogStringL(const char (&s)[N]) : ALogString(s, N - 1) {
    }
};

struct ALogStringPChar : public ALogString {
    using ALogString::ALogString;
};

// use ENABLE_IF_NOT_SAME, so that string literals (char[]) are not matched as 'const char*&' (T ==
// char)
template <typename T, ENABLE_IF_NOT_SAME(T, char *)>
inline const T &alog_forwarding(const T &x) {
    return x;
}

// matches string literals and const char[], but not char[]
template <size_t N>
inline ALogStringL alog_forwarding(const char (&s)[N]) {
    return ALogStringL(s);
}

// matches named char[] (not const!)
template <size_t N>
inline ALogStringPChar alog_forwarding(char (&s)[N]) {
    return ALogStringPChar(s, strlen(s));
}

// matches (const) char*
// we have to use the template form, otherwise it will override the templates of char[N]
template <typename T, ENABLE_IF_SAME(T, char)>
inline ALogStringPChar alog_forwarding(const T *const &s) {
    return ALogStringPChar(s, s ? strlen(s) : 0);
}

struct ALogBuffer {
    int level;
    char *ptr;
    uint32_t size;
    uint32_t reserved;
    void consume(size_t n) {
        ptr += n;
        size -= n;
    }
};

#define ALOG_DEBUG 0
#define ALOG_INFO 1
#define ALOG_WARN 2
#define ALOG_ERROR 3
#define ALOG_FATAL 4
#define ALOG_METRC 5
#define ALOG_AUDIT 6

class LogFormatter {
public:
    void put(ALogBuffer &buf, char c) {
        if (buf.size == 0)
            return;
        *buf.ptr = c;
        buf.consume(1);
    }

    void put(ALogBuffer &buf, const ALogString &s) {
        if (buf.size < s.size)
            return;
        memcpy(buf.ptr, s.s, s.size);
        buf.consume(s.size);
    }

    void put(ALogBuffer &buf, void *p) {
        put(buf, HEX((uint64_t)p).width(16));
    }

    template <typename T>
    using extend_to_64 =
        typename std::conditional<std::is_signed<T>::value, int64_t, uint64_t>::type;

    template <typename T, ENABLE_IF(std::is_integral<T>::value)>
    void put(ALogBuffer &buf, T x, int = 0) {
        const uint32_t upper_bound = sizeof(x) * 3;
        if (buf.size < upper_bound)
            return;
        put_integer(buf, extend_to_64<T>(x));
    }

    template <typename T, ENABLE_IF(std::is_enum<T>::value)>
    void put(ALogBuffer &buf, T x, void * = 0) {
        put_integer(buf, (int64_t)x);
    }

    void put(ALogBuffer &buf, ALogInteger x) {
        auto s = x.shift();
        if (s == 1 || s == 3 || s == 4)
            put_integer_hbo(buf, x);
        else /* if (s == 10) */
            put_integer_dec(buf, x);
    }

    void put(ALogBuffer &buf, double x) {
        snprintf(buf, "%0.4f", x);
    }

    void put(ALogBuffer &buf, FP x);

protected:
    void snprintf(ALogBuffer &buf, const char *fmt, double x) {
        int ret = ::snprintf(buf.ptr, buf.size, fmt, x);
        if (ret < 0)
            return;
        buf.consume(ret);
    }

    void put_integer_hbo(ALogBuffer &buf, ALogInteger x);
    void put_integer_dec(ALogBuffer &buf, ALogInteger x);
    void put_integer(ALogBuffer &buf, uint64_t x);
    void put_integer(ALogBuffer &buf, int64_t x) {
        uint64_t xx;
        if (x < 0) {
            put(buf, '-');
            xx = (uint64_t)-x;
        } else
            xx = (uint64_t)x;
        put_integer(buf, xx);
    }
};

struct LogBuffer;
struct ALogLogger {
    int log_level = ALOG_DEBUG;
    ILogOutput *log_output = log_output_stdout;

    ALogLogger(int level = 0, ILogOutput *output = log_output_stdout)
        : log_level(level), log_output(output) {
    }

    template <typename T>
    ALogLogger &operator<<(T &&rhs) {
        rhs.logger = this;
        return *this;
    }
};

extern ALogLogger default_logger;
extern ALogLogger default_audit_logger;

inline int get_log_file_fd() {
    return default_logger.log_output->get_log_file_fd();
}

extern int &log_output_level;
extern ILogOutput *&log_output;

static LogFormatter log_formatter;

class LogBuffer : public ALogBuffer {
public:
    LogBuffer(ILogOutput *output) : log_output(output) {
        ptr = buf;
        size = sizeof(buf) - 2;
    }
    ~LogBuffer() {
        printf('\n');
        *ptr = '\0';
        log_output->write(level, buf, ptr);
    }
    template <typename T>
    LogBuffer &operator<<(const T &x) {
        log_formatter.put(*this, alog_forwarding(x));
        return *this;
    }
    LogBuffer &printf() {
        return *this;
    }
    template <typename T, typename... Ts>
    LogBuffer &printf(const T &x, const Ts &...xs) {
        (*this) << x;
        return printf(xs...);
    }

protected:
    char buf[LOG_BUFFER_SIZE];
    ILogOutput *log_output;
    void operator=(const LogBuffer &rhs);
    LogBuffer(const LogBuffer &rhs);
};

namespace alog_format {
template <int... Is>
struct seq {
    template <int J>
    using push_back = seq<Is..., J>;
};

constexpr static char TOKEN = '`';

template <typename S, int I>
struct parser : public S {
    static_assert(I > 0, "...");
    static_assert(S::string()[I] != '\\', "'\\' in format string is illegal!");
    using S::LEN;
    using S::string;
    using typename S::type;

    constexpr static bool cond0 = (string()[I] != TOKEN);
    constexpr static bool cond1 = (string()[I - 1] != TOKEN);

    typedef typename parser<S, I - 1>::sequence prev_seq;
    typedef typename parser<S, I - 2>::sequence pprev_seq;

    using sequence = typename std::conditional<
        cond0, prev_seq,
        typename std::conditional<cond1, typename prev_seq::template push_back<I - 1>,
                                  typename pprev_seq::template push_back<-(I - 1)>>::type>::type;
};

template <typename S>
struct parser<S, -1> {
    typedef seq<> sequence;
};

template <typename S>
struct parser<S, 0> : public S {
    static_assert(S::string()[0] != '\\', "'\\' in format string is illegal!");
    constexpr static bool cond = (S::string()[0] != TOKEN);
    typedef typename std::conditional<cond, seq<>, seq<0>>::type sequence;
};

template <typename SEQ>
struct ALogFMTString : public ALogStringL {
    using ALogStringL::ALogStringL;
};

class FMTLogBuffer;
template <bool B, int BEGIN>
struct next {
    template <typename FMT, typename T, typename... Ts>
    static inline FMTLogBuffer &printf_fmt(FMTLogBuffer &log, FMT fmt, const T &x, const Ts &...xs);

    template <typename FMT>
    static inline FMTLogBuffer &printf_fmt(FMTLogBuffer &log, FMT fmt);
};

template <int BEGIN>
struct next<false, BEGIN> {
    template <typename FMT, typename T, typename... Ts>
    static inline FMTLogBuffer &printf_fmt(FMTLogBuffer &log, FMT fmt, const T &x, const Ts &...xs);

    template <typename FMT>
    static inline FMTLogBuffer &printf_fmt(FMTLogBuffer &log, FMT fmt);
};

class FMTLogBuffer : public LogBuffer {
public:
    using LogBuffer::printf;

    FMTLogBuffer(ILogOutput *output) : LogBuffer(output) {
    }

    template <int BEGIN, typename SEQ>
    FMTLogBuffer &printf_fmt(ALogFMTString<SEQ> fmt) {
        printf(fmt);
        return *this;
    }
    template <int BEGIN, typename T, typename... Ts>
    FMTLogBuffer &printf_fmt(ALogFMTString<seq<>> fmt, const T &x, const Ts &...xs) {
        printf(fmt, x, xs...);
        return *this;
    }
    template <int BEGIN, int I, int... Is, typename T, typename... Ts>
    FMTLogBuffer &printf_fmt(ALogFMTString<seq<I, Is...>> fmt, const T &x, const Ts &...xs);
};

template <bool B, int BEGIN>
template <typename FMT, typename T, typename... Ts>
FMTLogBuffer &next<B, BEGIN>::printf_fmt(FMTLogBuffer &log, FMT fmt, const T &x, const Ts &...xs) {
    log.printf(x);
    return log.printf_fmt<BEGIN>(fmt, xs...);
}

template <bool B, int BEGIN>
template <typename FMT>
FMTLogBuffer &next<B, BEGIN>::printf_fmt(FMTLogBuffer &log, FMT fmt) {
    return log.printf(fmt);
}

template <int BEGIN>
template <typename FMT, typename T, typename... Ts>
FMTLogBuffer &next<false, BEGIN>::printf_fmt(FMTLogBuffer &log, FMT fmt, const T &x,
                                             const Ts &...xs) {
    return log.printf_fmt<BEGIN>(fmt, x, xs...);
}

template <int BEGIN>
template <typename FMT>
FMTLogBuffer &next<false, BEGIN>::printf_fmt(FMTLogBuffer &log, FMT fmt) {
    return log.printf(fmt);
}

template <int BEGIN, int I, int... Is, typename T, typename... Ts>
FMTLogBuffer &FMTLogBuffer::printf_fmt(ALogFMTString<seq<I, Is...>> fmt, const T &x,
                                       const Ts &...xs) {
    const int POS = I > 0 ? I : -I;
    static_assert(POS >= BEGIN, "...");
    const size_t L = POS - BEGIN;

    assert(L < fmt.size);
    printf(ALogString(fmt.s, L));

    ALogFMTString<seq<Is...>> fmt_next(fmt.s + (L + 1), fmt.size - (L + 1));
    return next<(I >= 0), (POS + 1)>::printf_fmt(*this, fmt_next, x, xs...);
}
} // namespace alog_format

struct Prologue {
    uint64_t addr_func, addr_file;
    int len_func, len_file;
    int line, level;
};

LogBuffer &operator<<(LogBuffer &log, const Prologue &pro);

template <typename SEQ = int, typename... Ts>
__attribute__((noinline)) static void __do_log__(int level, ILogOutput *output,
                                                 const Prologue &prolog, const Ts &...xs) {
    LogBuffer log(output);
    log << prolog;
    log.printf(xs...);
    log.level = level;
}

template <typename SEQ = int, typename... Ts>
inline void __log__(int level, ILogOutput *output, const Prologue &prolog, Ts &&...xs) {
    __do_log__(level, output, prolog, alog_forwarding(xs)...);
}

template <typename SEQ = int, size_t N, typename... Ts>
inline void __log__(int level, ILogOutput *output, const Prologue &prolog, char (&s)[N],
                    Ts &&...xs) {
    __do_log__(level, output, prolog, alog_forwarding(s), alog_forwarding(xs)...);
}

template <typename SEQ, size_t N, typename... Ts>
__attribute__((noinline)) static void __log__(int level, ILogOutput *output, const Prologue &prolog,
                                              const char (&fmt)[N], Ts &&...xs) {
    alog_format::FMTLogBuffer log(output);
    log << prolog;
    log.printf_fmt<0>(alog_format::ALogFMTString<SEQ>(fmt), alog_forwarding(xs)...);
    log.level = level;
}

template <typename BuilderType>
struct LogBuilder {
    int level;
    BuilderType builder;
    ALogLogger *logger;
    bool done;

    LogBuilder(int level, BuilderType builder)
        : level(level), builder(builder), logger(&default_logger), done(false) {
    }
    LogBuilder(LogBuilder &&rhs)
        : level(rhs.level), builder(rhs.builder), logger(rhs.logger), done(rhs.done) {
        // it might copy
        // so just make sure the other one will not output
        rhs.done = true;
    }
    ~LogBuilder() {
        if (!done && level >= logger->log_level) {
            builder(logger->log_output);
            done = true;
        }
    }
};

#define DEFINE_PROLOGUE(level, prolog)                                                             \
    const static Prologue prolog{(uint64_t) __func__,  (uint64_t)__FILE__, sizeof(__func__) - 1,   \
                                 sizeof(__FILE__) - 1, __LINE__,           level};

#define PARSE_FMTSTR(S, thesequence)                                                               \
    struct static_string {                                                                         \
        enum { LEN = sizeof(#S) - 1 };                                                             \
        typedef const char (&type)[LEN + 1];                                                       \
        constexpr static type string() {                                                           \
            return #S;                                                                             \
        }                                                                                          \
    };                                                                                             \
    typedef typename alog_format::parser<static_string, static_string::LEN>::sequence thesequence;

#define __LOG__(level, first, ...)                                                                 \
    ({                                                                                             \
        DEFINE_PROLOGUE(level, prolog);                                                            \
        auto __build_lambda__ = [&](ILogOutput *__output_##__LINE__) {                             \
            PARSE_FMTSTR(first, sequence);                                                         \
            __log__<sequence>(level, __output_##__LINE__, prolog, first, __VA_ARGS__);             \
        };                                                                                         \
        LogBuilder<decltype(__build_lambda__)>(level, __build_lambda__);                           \
    })

struct alog_noop {};
inline LogBuffer &operator<<(LogBuffer &log, const alog_noop &) {
    return log;
}

#define LOG_DEBUG(...) (__LOG__(ALOG_DEBUG, __VA_ARGS__, alog_noop()))
#define LOG_INFO(...) (__LOG__(ALOG_INFO, __VA_ARGS__, alog_noop()))
#define LOG_WARN(...) (__LOG__(ALOG_WARN, __VA_ARGS__, alog_noop()))
#define LOG_ERROR(...) (__LOG__(ALOG_ERROR, __VA_ARGS__, alog_noop()))
#define LOG_FATAL(...) (__LOG__(ALOG_FATAL, __VA_ARGS__, alog_noop()))
#define LOG_METRC(...) (__LOG__(ALOG_METRC, __VA_ARGS__, alog_noop()))
#define LOG_AUDIT(...) (__LOG__(ALOG_AUDIT, __VA_ARGS__, alog_noop()))

inline void set_log_output(ILogOutput *output) {
    default_logger.log_output = output;
}

inline void set_log_output_level(int l) {
    default_logger.log_level = l;
}

struct ERRNO {
    const int no;
    ERRNO() : no(errno) {
    }
    constexpr ERRNO(int no) : no(no) {
    }
};

inline LogBuffer &operator<<(LogBuffer &log, ERRNO e) {
    auto no = e.no ? e.no : errno;
    return log.printf("errno=", no, '(', strerror(no), ')');
}

template <typename T>
struct NamedValue {
    ALogStringL name;
    T &&value;
};

template <size_t N, typename T>
inline NamedValue<T> make_named_value(const char (&name)[N], T &&value) {
    return NamedValue<T>{ALogStringL(name), std::forward<T>(value)};
}

#define VALUE(x) make_named_value(#x, x)

template <typename T>
inline LogBuffer &operator<<(LogBuffer &log, const NamedValue<T> &v) {
    return log.printf('[', v.name, '=', v.value, ']');
}

inline LogBuffer &operator<<(LogBuffer &log, const NamedValue<ALogString> &v) {
    return log.printf('[', v.name, '=', '"', v.value, '"', ']');
}

// output a log message, set errno, then return a value
// keep errno unchanged if new_errno == 0
#define LOG_ERROR_RETURN(new_errno, retv, ...)                                                     \
    {                                                                                              \
        int xcode = (int)(new_errno);                                                              \
        if (xcode == 0)                                                                            \
            xcode = errno;                                                                         \
        LOG_ERROR(__VA_ARGS__);                                                                    \
        errno = xcode;                                                                             \
        return retv;                                                                               \
    }

// output a log message with errno info, set errno, then return a value
// keep errno unchanged if new_errno == 0
#define LOG_ERRNO_RETURN(new_errno, retv, ...)                                                     \
    {                                                                                              \
        ERRNO eno;                                                                                 \
        LOG_ERROR(__VA_ARGS__, ' ', eno);                                                          \
        errno = (new_errno) ? (new_errno) : eno.no;                                                \
        return retv;                                                                               \
    }
