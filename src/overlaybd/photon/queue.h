/*
  Copyright (c) 2021 wujiaxu <void00@foxmail.com>

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
#include <stdlib.h>
#include <string.h>
#include <utility>

template <typename T, unsigned int capacity>
class __spsc_queue; // not for user

// replace boost/lockfree/spsc_queue.hpp
template <typename T, unsigned int capacity>
class spsc_queue
{
public:
    spsc_queue()  { }
    ~spsc_queue() { }
    spsc_queue(const spsc_queue&) = delete;
    spsc_queue(spsc_queue&&) = delete;
    spsc_queue& operator=(const spsc_queue&) = delete;
    spsc_queue& operator=(spsc_queue&&) = delete;

public:
    int read_available() const;

    bool push(const T& t);
    bool push(T&& t);
    bool pop(T& ret);

    int push(const T *ret, int n);
    int pop(T *ret, int n);

private:
    __spsc_queue<T, capacity> queue_;
};

////
// template inl, not for user
template <typename T, unsigned int capacity>
int spsc_queue<T, capacity>::read_available() const
{
    return queue_.read_available();
}

template <typename T, unsigned int capacity>
bool spsc_queue<T, capacity>::push(const T& t)
{
    return queue_.push(t);
}

template <typename T, unsigned int capacity>
bool spsc_queue<T, capacity>::push(T&& t)
{
    return queue_.push(std::move(t));
}

template <typename T, unsigned int capacity>
bool spsc_queue<T, capacity>::pop(T& t)
{
    return queue_.pop(t);
}

template <typename T, unsigned int capacity>
int spsc_queue<T, capacity>::push(const T *ret, int n)
{
    return queue_.push(ret, n);
}

template <typename T, unsigned int capacity>
int spsc_queue<T, capacity>::pop(T *ret, int n)
{
    return queue_.pop(ret, n);
}

struct __fifo
{
    unsigned int in;
    unsigned int out;
    unsigned int mask;
    unsigned int size;
    void *buffer;
};

template <typename T, bool is_trivial = std::is_trivial<T>::value>
class __spsc_worker;

#define __CHECK_POWER_OF_2(x) ((x) > 0 && ((x) & ((x) - 1)) == 0)

template <typename T, unsigned int capacity>
class __spsc_queue
{
public:
    __spsc_queue();
    ~__spsc_queue() { }
    __spsc_queue(const __spsc_queue&) = delete;
    __spsc_queue(__spsc_queue&&) = delete;
    __spsc_queue& operator=(const __spsc_queue&) = delete;
    __spsc_queue& operator=(__spsc_queue&&) = delete;

public:
    int read_available() const;

    bool push(const T& t);
    bool push(T&& t);
    bool pop(T& ret);

    int push(const T *ret, int n);
    int pop(T *ret, int n);

private:
    __fifo fifo_;
    T arr_[capacity];

    using WORKER = __spsc_worker<T, std::is_trivial<T>::value>;
    static_assert(__CHECK_POWER_OF_2(capacity), "Capacity MUST power of 2");
};

template <typename T, unsigned int capacity>
__spsc_queue<T, capacity>::__spsc_queue()
{
    fifo_.in = 0;
    fifo_.out = 0;
    fifo_.mask = capacity - 1;
    fifo_.size = capacity;
    fifo_.buffer = &arr_;
}

template <typename T, unsigned int capacity>
int __spsc_queue<T, capacity>::read_available() const
{
    return fifo_.in - fifo_.out;
}

template <typename T, unsigned int capacity>
bool __spsc_queue<T, capacity>::push(const T& t)
{
    if (capacity - fifo_.in + fifo_.out == 0)
        return false;

    arr_[fifo_.in & (capacity - 1)] = t;

    asm volatile("sfence" ::: "memory");

    ++fifo_.in;

    return true;
}

template <typename T, unsigned int capacity>
bool __spsc_queue<T, capacity>::push(T&& t)
{
    if (capacity - fifo_.in + fifo_.out == 0)
        return false;

    arr_[fifo_.in & (capacity - 1)] = std::move(t);

    asm volatile("sfence" ::: "memory");

    ++fifo_.in;

    return true;
}

template <typename T, unsigned int capacity>
bool __spsc_queue<T, capacity>::pop(T& t)
{
    if (fifo_.in - fifo_.out == 0)
        return false;

    t = std::move(arr_[fifo_.out & (capacity - 1)]);

    asm volatile("sfence" ::: "memory");

    ++fifo_.out;

    return true;
}

template <typename T, unsigned int capacity>
int __spsc_queue<T, capacity>::push(const T *ret, int n)
{
    return WORKER::push(&fifo_, ret, n);
}

template <typename T, unsigned int capacity>
int __spsc_queue<T, capacity>::pop(T *ret, int n)
{
    return WORKER::pop(&fifo_, ret, n);
}

static inline unsigned int _min(unsigned int a, unsigned int b)
{
    return (a < b) ? a : b;
}

template <typename T>
class __spsc_worker<T, true>
{
public:
    static int push(__fifo *fifo, const T *ret, int n)
    {
        unsigned int len = _min(n, fifo->size - fifo->in + fifo->out);
        if (len == 0)
            return 0;

        unsigned int idx_in = fifo->in & fifo->mask;
        unsigned int l = _min(len, fifo->size - idx_in);
        T *arr = (T *)fifo->buffer;

        memcpy(arr + idx_in, ret, l * sizeof (T));
        memcpy(arr, ret + l, (len - l) * sizeof (T));

        asm volatile("sfence" ::: "memory");

        fifo->in += len;

        return len;
    }

    static int pop(__fifo *fifo, T *ret, int n)
    {
        unsigned int len = _min(n, fifo->in - fifo->out);
        if (len == 0)
            return 0;

        unsigned int idx_out = fifo->out & fifo->mask;
        unsigned int l = _min(len, fifo->size - idx_out);
        T *arr = (T *)fifo->buffer;

        memcpy(ret, arr + idx_out, l * sizeof (T));
        memcpy(ret + l, arr, (len - l) * sizeof (T));

        asm volatile("sfence" ::: "memory");

        fifo->out += len;

        return len;
    }
};

template <typename T>
class __spsc_worker<T, false>
{
public:
    static int push(__fifo *fifo, const T *ret, int n)
    {
        unsigned int len = _min(n, fifo->size - fifo->in + fifo->out);
        if (len == 0)
            return 0;

        unsigned int idx_in = fifo->in & fifo->mask;
        unsigned int l = _min(len, fifo->size - idx_in);
        T *arr = (T *)fifo->buffer;

        for (unsigned int i = 0; i < l; i++)
            arr[idx_in + i] = ret[i];

        for (unsigned int i = 0; i < len - l; i++)
            arr[i] = ret[l + i];

        asm volatile("sfence" ::: "memory");

        fifo->in += len;

        return len;
    }

    static int pop(__fifo *fifo, T *ret, int n)
    {
        unsigned int len = _min(n, fifo->in - fifo->out);
        if (len == 0)
            return 0;

        unsigned int idx_out = fifo->out & fifo->mask;
        unsigned int l = _min(len, fifo->size - idx_out);
        T *arr = (T *)fifo->buffer;

        for (unsigned int i = 0; i < l; i++)
            ret[i] = std::move(arr[idx_out + i]);

        for (unsigned int i = 0; i < len - l; i++)
            ret[l + i] = std::move(arr[i]);

        asm volatile("sfence" ::: "memory");

        fifo->out += len;

        return len;
    }
};
