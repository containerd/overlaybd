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
#include "kfifo.h"

// replace boost/lockfree/spsc_queue.hpp
template <typename T, uint32_t capacity>
class spsc_queue
{
protected:
    static constexpr unsigned int ptrSize = sizeof(T*);

public:
    spsc_queue()
    {
        fifo_ = kfifo_alloc(capacity * ptrSize);
        if (!fifo_)
            abort();
    }

    ~spsc_queue()
    {
        kfifo_free(fifo_);
    }

    bool push(const T& t)
    {
        return __kfifo_put(fifo_, &t, ptrSize) > 0;
    }

    int pop(T* ret, int n)
    {
        return __kfifo_get(fifo_, ret, ptrSize * n) / ptrSize;
    }

    int read_available() const
    {
        return __kfifo_len(fifo_) / ptrSize;
    }

    int read_available()
    {
        return kfifo_len(fifo_) / ptrSize;
    }

private:
    struct kfifo* fifo_;
};

// lock-free multiple-producer/multiple-consumer ringbuffer
template <typename T, uint32_t capacity>
class mpmc_queue
{
protected:
    static constexpr unsigned int ptrSize = sizeof(T*);

public:
    mpmc_queue()
    {
        fifo_ = kfifo_alloc(capacity * ptrSize);
        if (!fifo_)
            abort();
    }

    ~mpmc_queue()
    {
        kfifo_free(fifo_);
    }

    bool push(const T& t)
    {
        return kfifo_put(fifo_, &t, ptrSize) > 0;
    }

    int pop(T* ret, int n)
    {
        return kfifo_get(fifo_, ret, ptrSize * n) / ptrSize;
    }

    int read_available() const
    {
        return __kfifo_len(fifo_) / ptrSize;
    }

    int read_available()
    {
        return kfifo_len(fifo_) / ptrSize;
    }

private:
    struct kfifo* fifo_;
};

