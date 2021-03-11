/*
 * async_stream.h
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
#include <sys/types.h>
#include "async.h"

class IAsyncStream : public IAsyncBase {
public:
    DEFINE_ASYNC0(int, close);

    DEFINE_ASYNC(ssize_t, read, void *buf, size_t count);
    DEFINE_ASYNC(ssize_t, readv, const struct iovec *iov, int iovcnt);
    EXPAND_FUNC(ssize_t, readv_mutable, struct iovec *iov, int iovcnt) {
        readv(iov, iovcnt, done, timeout);
    }

    DEFINE_ASYNC(ssize_t, write, const void *buf, size_t count);
    DEFINE_ASYNC(ssize_t, writev, const struct iovec *iov, int iovcnt);
    EXPAND_FUNC(ssize_t, writev_mutable, struct iovec *iov, int iovcnt) {
        writev(iov, iovcnt, done, timeout);
    }

    const static uint32_t OPID_CLOSE = 0;
    const static uint32_t OPID_READ = 1;
    const static uint32_t OPID_READV = 2;
    const static uint32_t OPID_WRITE = 3;
    const static uint32_t OPID_WRITEV = 4;

    using FuncIO = AsyncFunc<ssize_t, IAsyncStream, void *, size_t>;
    FuncIO _and_read() {
        return &IAsyncStream::read;
    }
    FuncIO _and_write() {
        return (FuncIO)&IAsyncStream::write;
    }
    bool is_readf(FuncIO f) {
        return f == _and_read();
    }
    bool is_writef(FuncIO f) {
        return f == _and_write();
    }

    using FuncIOV_mutable = AsyncFunc<ssize_t, IAsyncStream, struct iovec *, int>;
    FuncIOV_mutable _and_readv_mutable() {
        return &IAsyncStream::readv_mutable;
    }
    FuncIOV_mutable _and_writev_mutable() {
        return &IAsyncStream::writev_mutable;
    }
    bool is_readf_mutable(FuncIOV_mutable f) {
        return f == _and_readv_mutable();
    }
    bool is_writef_mutable(FuncIOV_mutable f) {
        return f == _and_writev_mutable();
    }

    using FuncIOCV = AsyncFunc<ssize_t, IAsyncStream, const struct iovec *, int>;
    FuncIOCV _and_readcv() {
        return &IAsyncStream::readv;
    }
    FuncIOCV _and_writecv() {
        return &IAsyncStream::writev;
    }
    bool is_readf(FuncIOCV f) {
        return f == _and_readcv();
    }
    bool is_writef(FuncIOCV f) {
        return f == _and_writecv();
    }
};

//////////////////////////////////////////////////////////////////////////////////////////////////
class Example_of_Async_Operation {
public:
    IAsyncStream *m_astream;
    void do_async_pread(void *buf, size_t count) {
        // this->on_read_done(aop) will be called upon completion
        m_astream->read(buf, count, {this, &Example_of_Async_Operation::on_read_done});
    }

protected:
    int on_read_done(AsyncResult<ssize_t> *aop) {
        if (aop->result < 0) {
            printf("[%p].async_read() is failed, with result=%d, and errno=%d, %s\n", aop->object,
                   (int)aop->result, aop->error_number, strerror(aop->error_number));
            return -1;
        }

        printf("[%p].async_read() is successfully done, with result=%d", aop->object,
               (int)aop->result);
        return 0;
    }
    const char *strerror(int e) {
        return "some error message";
    }
    void printf(...) {
    }
};
