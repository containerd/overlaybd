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
#include <pthread.h>
#include <stdlib.h>
#include <utility>
#include "kfifo.h"

template<typename T, unsigned int capacity, bool = std::is_pod<T>::value>
class __spsc_queue;

template<typename T, unsigned int capacity, bool = std::is_pod<T>::value>
class __mpmc_queue;

// replace boost/lockfree/spsc_queue.hpp
template <typename T, unsigned int capacity>
class spsc_queue
{
public:
    bool push(const T& t)
    {
        return queue_.push(t);
    }

    bool push(T&& t)
    {
        return queue_.push(std::move(t));
    }

    int pop(T* ret, int n)
    {
        return queue_.pop(ret, n);
    }

    int read_available() const
    {
        return queue_.read_available();
    }

private:
    __spsc_queue<T, capacity> queue_;
};

// thread-safety multiple-producer/multiple-consumer circular-queue
template <typename T, unsigned int capacity>
class mpmc_queue
{
public:
    bool push(const T& t)
    {
        return queue_.push(t);
    }

    bool push(T&& t)
    {
        return queue_.push(std::move(t));
    }

    int pop(T* ret, int n)
    {
        return queue_.pop(ret, n);
    }

    int read_available() const
    {
        return queue_.read_available();
    }

private:
    __mpmc_queue<T, capacity> queue_;
};

//// inner template
// not for user

/* T is POD. */
template<typename T, unsigned int capacity>
class __spsc_queue<T, capacity, true>
{
protected:
    static constexpr unsigned int sizeT = sizeof(T);

public:
    __spsc_queue()
    {
        fifo_ = kfifo_alloc(capacity * sizeT);
        if (!fifo_)
            abort();
    }

    ~__spsc_queue()
    {
        kfifo_free(fifo_);
    }

    bool push(const T& t)
    {
        return __kfifo_put(fifo_, &t, sizeT) > 0;
    }

    bool push(T&& t)
    {
        return __kfifo_put(fifo_, &t, sizeT) > 0;
    }

    int pop(T* ret, int n)
    {
        return __kfifo_get(fifo_, ret, sizeT * n) / sizeT;
    }

    int read_available() const
    {
        return __kfifo_len(fifo_) / sizeT;
    }

private:
    struct kfifo* fifo_;
};

/* T is not POD. */
template<typename T, unsigned int size>
class __spsc_queue<T, size, false>
{
public:
    bool push(const T& t)
    {
        if (size - in_ + out_ == 0)
            return 0;

        buffer_[(in_++) & (size - 1)] = t;
        return 1;
    }

    bool push(T&& t)
    {
        if (size - in_ + out_ == 0)
            return 0;

        buffer_[(in_++) & (size - 1)] = std::move(t);
        return 1;
    }

    int pop(T* ret, int n)
    {
        unsigned int l;
        unsigned int len = n;

        len = _min(len, in_ - out_);

        asm volatile("lfence" ::: "memory");

        l = _min(len, size - (out_ & (size - 1)));

        for (unsigned int i = 0; i < l; i++)
            ret[i] = std::move(buffer_[(out_ & (size - 1)) + i]);

        for (unsigned int i = 0; i < len - l; i++)
            ret[l + i] = std::move(buffer_[i]);

        asm volatile("mfence" ::: "memory");

        out_ += len;

        return len;
    }

    int read_available() const
    {
        return in_ - out_;
    }

private:
    T buffer_[size];
    unsigned int in_ = 0;
    unsigned int out_ = 0;
};

/* T is POD. */
template<typename T, unsigned int capacity>
class __mpmc_queue<T, capacity, true>
{
protected:
    static constexpr unsigned int sizeT = sizeof(T);

public:
    __mpmc_queue()
    {
        fifo_ = kfifo_alloc(capacity * sizeT);
        if (!fifo_)
            abort();
    }

    ~__mpmc_queue()
    {
        kfifo_free(fifo_);
    }

    bool push(const T& t)
    {
        return kfifo_put(fifo_, &t, sizeT) > 0;
    }

    bool push(T&& t)
    {
        return kfifo_put(fifo_, &t, sizeT) > 0;
    }

    int pop(T* ret, int n)
    {
        return kfifo_get(fifo_, ret, sizeT * n) / sizeT;
    }

    int read_available() const
    {
        return __kfifo_len(fifo_) / sizeT;
    }

private:
    struct kfifo* fifo_;
};

/* T is not POD. */
template<typename T, unsigned int capacity>
class __mpmc_queue<T, capacity, false>
{
public:
    __mpmc_queue()
    {
        if (pthread_spin_init(&lock_, PTHREAD_PROCESS_PRIVATE) != 0)
            abort();
    }

    ~__mpmc_queue()
    {
        pthread_spin_destroy(&lock_);
    }

    bool push(const T& t)
    {
        pthread_spin_lock(&lock_);
        bool res = queue_.push(t);
        pthread_spin_unlock(&lock_);
        return res;
    }

    bool push(T&& t)
    {
        pthread_spin_lock(&lock_);
        bool res = queue_.push(std::move(t));
        pthread_spin_unlock(&lock_);
        return res;
    }

    int pop(T* ret, int n)
    {
        pthread_spin_lock(&lock_);
        int res = queue_.pop(ret, n);
        pthread_spin_unlock(&lock_);
        return res;
    }

    int read_available() const
    {
        // no need lock
        return queue_.read_available();
    }

private:
    pthread_spinlock_t lock_;
    __spsc_queue<T, capacity> queue_;
};
