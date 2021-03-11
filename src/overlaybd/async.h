/*
 * async.h
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
#include <sys/types.h>
#include <errno.h>
#include <utility>
#include <tuple>
#include <type_traits>
#include "object.h"
#include "callback.h"

template <typename T>
struct _AsyncResult {
    typedef T result_type;
    void *object;       // the object that performed the async operation
    uint32_t operation; // ID of the operation
    int error_number;   // errno, available only in case of error
    T result;           // result of the operation
    T get_result() {
        return result;
    }
};

template <typename T>
struct AsyncResult : public _AsyncResult<T> {
    bool is_failure() {
        return this->result < 0;
    }
    static T failure() {
        return -1;
    }
};

template <>
struct AsyncResult<void> : public _AsyncResult<char> {
    bool is_failure() {
        return false;
    }
    static char failure() {
        return 0;
    }
    void get_result() {
    }
};

template <typename Obj>
struct AsyncResult<Obj *> : public _AsyncResult<Obj *> {
    bool is_failure() {
        return this->result == nullptr;
    }
    static Obj *failure() {
        return nullptr;
    }
};

// When an I/O operation is done, `done` will be called with an `AsyncResult<T>*`
template <typename T>
using Done = Callback<AsyncResult<T> *>;

template <typename T>
struct done_traits;

template <typename T>
struct done_traits<Done<T>> {
    using result_type = T;
};

template <typename R, typename T, typename... ARGS>
using AsyncFunc = void (T::*)(ARGS..., Done<R>, uint64_t timeout);

template <typename F>
struct af_traits;

template <typename T, typename... ARGS>
struct af_traits<void (T::*)(ARGS...)> {
    // using result_type = R;
    using interface_type = T;
    using args_type = std::tuple<ARGS...>;
    const static int nargs = sizeof...(ARGS);
    static_assert(nargs >= 2, "...");
    using done_type = typename std::tuple_element<nargs - 2, args_type>::type;
    using result_type = typename done_traits<done_type>::result_type;
};

// async functions have 2 additional arguments: `done` for callback, and `timeout` for timeout
#define EXPAND_FUNC(T, OP, ...) virtual void OP(__VA_ARGS__, Done<T> done, uint64_t timeout = -1)
#define EXPAND_FUNC0(T, OP) virtual void OP(Done<T> done, uint64_t timeout = -1)

// used for define async functions in interfaces
#define DEFINE_ASYNC(T, OP, ...) EXPAND_FUNC(T, OP, __VA_ARGS__) = 0
#define DEFINE_ASYNC0(T, OP) EXPAND_FUNC0(T, OP) = 0

// used for override and implement async functions in concrete classes
#define OVERRIDE_ASYNC(T, OP, ...) EXPAND_FUNC(T, OP, __VA_ARGS__) override
#define OVERRIDE_ASYNC0(T, OP) EXPAND_FUNC0(T, OP) override

#define UNIMPLEMENTED_(T)                                                                          \
    { callback_umimplemented(done); }

// used for define async functions with a default (un)-implementation
#define UNIMPLEMENTED_ASYNC(T, OP, ...) EXPAND_FUNC(T, OP, __VA_ARGS__) UNIMPLEMENTED_(T)
#define UNIMPLEMENTED_ASYNC0(T, OP) EXPAND_FUNC0(T, OP) UNIMPLEMENTED_(T)

// used for override as unimplemented async functions in concrete classes
#define OVERRIDE_UNIMPLEMENTED_ASYNC(T, OP, ...)                                                   \
    OVERRIDE_ASYNC(T, OP, __VA_ARGS__) UNIMPLEMENTED_(T)
#define OVERRIDE_UNIMPLEMENTED_ASYNC0(T, OP) OVERRIDE_ASYNC0(T, OP) UNIMPLEMENTED_(T)

class IAsyncBase : public Object {
protected:
    template <typename T>
    void callback(Done<T> done, uint32_t operation, T ret, int error_number) {
        AsyncResult<T> r;
        r.object = this;
        r.operation = operation;
        r.error_number = error_number;
        r.result = ret;
        done(&r);
    }
    template <typename T>
    void callback_umimplemented(Done<T> done) {
        callback(done, UINT32_MAX, AsyncResult<T>::failure(), ENOSYS);
    }
};

namespace photon {
struct thread;
extern thread *CURRENT;
int thread_usleep(uint64_t useconds);
extern "C" void safe_thread_interrupt(thread *, int, int);
} // namespace photon

// wraps an *AsyncFunc* running in another kernel thread,
// so that it is callable from a photon thread
template <typename R, typename T, typename... ARGS>
class AsyncFuncWrapper {
public:
    using AFunc = AsyncFunc<R, T, ARGS...>;
    AsyncFuncWrapper(T *obj, AFunc afunc, uint64_t timeout)
        : _obj(obj), afunc_(afunc), _timeout(timeout) {
    }

    template <typename... _Args>
    R call(_Args &&...args) {
        AsyncReturn aret;
        (_obj->*afunc_)(std::forward<_Args>(args)..., aret.done(), _timeout);
        return aret.wait_for_result();
    }

protected:
    T *_obj;
    AFunc afunc_;
    uint64_t _timeout;

    struct AsyncReturn {
        photon::thread *th = photon::CURRENT;
        AsyncResult<R> result;
        bool gotit = false;
        Done<R> done() {
            return {this, &on_done};
        }
        R wait_for_result() {
            photon::thread_usleep(-1);
            assert(gotit);
            if (result.is_failure())
                errno = result.error_number;
            return result.get_result();
        }
        static int on_done(void *aret_, AsyncResult<R> *ar) {
            auto aret = (AsyncReturn *)aret_;
            aret->result = *ar;
            aret->gotit = true;
            photon::safe_thread_interrupt(aret->th, EINTR, 0);
            return 0;
        }
    };
};

template <typename R, typename T, typename... ARGS>
inline AsyncFuncWrapper<R, T, ARGS...> async_func(T *obj, AsyncFunc<R, T, ARGS...> afunc,
                                                  uint64_t timeout) {
    return {obj, afunc, timeout};
}

// Wraps a generic async func running in another kernel thread,
// so that it is callable from a photon thread.
// The completion of the func MUST invoke put_result(),
// so as to pass its result, and indicate its completion as well.
template <typename R>
class AsyncFuncWrapper_Generic {
public:
    template <typename AFunc>
    R call(AFunc afunc) {
        gotit = false;
        th = photon::CURRENT;
        afunc();
        photon::thread_usleep(-1);
        assert(gotit);
        if (result.is_failure() && result.error_number)
            errno = result.error_number;
        return result.get_result();
    }
    template <typename Rst>
    void put_result(Rst &&r, int error_number) {
        result.result = std::forward<Rst>(r);
        if (result.is_failure() && error_number)
            result.error_number = error_number;
        put_result();
    }
    void put_result(int error_number) {
        if (error_number != 0)
            result.error_number = error_number;
        put_result();
    }

protected:
    photon::thread *th;
    AsyncResult<R> result;
    bool gotit;

    void put_result() {
        gotit = true;
        photon::safe_thread_interrupt(th, EINTR, 0);
    }
};
