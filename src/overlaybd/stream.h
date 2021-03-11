/*
 * stream.h
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
#include "object.h"

struct iovec;

enum class ShutdownHow : int { Read = 0, Write = 1, ReadWrite = 2 };

class IStream : public Object {
public:
    virtual int close() = 0;

    virtual int shutdown(ShutdownHow how) {
        return 0;
    }

    virtual ssize_t read(void *buf, size_t count) = 0;
    virtual ssize_t readv(const struct iovec *iov, int iovcnt) = 0;
    virtual ssize_t
    readv_mutable(struct iovec *iov,
                  int iovcnt) { // there might be a faster implementaion in derived class
        return readv(iov, iovcnt);
    }

    virtual ssize_t write(const void *buf, size_t count) = 0;
    virtual ssize_t writev(const struct iovec *iov, int iovcnt) = 0;
    virtual ssize_t
    writev_mutable(struct iovec *iov,
                   int iovcnt) { // there might be a faster implementaion in derived class
        return writev(iov, iovcnt);
    }

    // member function pointer to either read() or write()
    typedef ssize_t (IStream::*FuncIO)(void *buf, size_t count);
    FuncIO _and_read() {
        return &IStream::read;
    }
    FuncIO _and_write() {
        return (FuncIO)&IStream::write;
    }
    bool is_readf(FuncIO f) {
        return f == _and_read();
    }
    bool is_writef(FuncIO f) {
        return f == _and_write();
    }

    // member function pointer to either readv() or writev(), the non-const iovec* edition
    typedef ssize_t (IStream::*FuncIOV_mutable)(struct iovec *iov, int iovcnt);
    FuncIOV_mutable _and_readv_mutable() {
        return &IStream::readv_mutable;
    }
    FuncIOV_mutable _and_writev_mutable() {
        return &IStream::writev_mutable;
    }
    bool is_readf(FuncIOV_mutable f) {
        return f == _and_readv_mutable();
    }
    bool is_writef(FuncIOV_mutable f) {
        return f == _and_writev_mutable();
    }

    // member function pointer to either readv() or writev(), the const iovec* edition
    typedef ssize_t (IStream::*FuncIOCV)(const struct iovec *iov, int iovcnt);
    FuncIOCV _and_readcv() {
        return &IStream::readv;
    }
    FuncIOCV _and_writecv() {
        return &IStream::writev;
    }
    bool is_readf(FuncIOCV f) {
        return f == _and_readcv();
    }
    bool is_writef(FuncIOCV f) {
        return f == _and_writecv();
    }
};
