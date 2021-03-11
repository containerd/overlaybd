/*
 * aio-wrapper.h
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
#include <sys/uio.h>

// aio wrapper depends on fd-events ( fd_events_epoll_init() )
namespace photon {
extern "C" {
int libaio_wrapper_init();
int libaio_wrapper_fini();

// `fd` must be opened with O_DIRECT, and the buffers must be aligned
ssize_t libaio_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t libaio_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
ssize_t libaio_pwrite(int fd, const void *buf, size_t count, off_t offset);
ssize_t libaio_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);
static int libaio_fsync(int fd) {
    return 0;
}

ssize_t posixaio_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t posixaio_pwrite(int fd, const void *buf, size_t count, off_t offset);
int posixaio_fsync(int fd);
int posixaio_fdatasync(int fd);
}

struct libaio {
    static ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
        return libaio_pread(fd, buf, count, offset);
    }
    static ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
        return libaio_preadv(fd, iov, iovcnt, offset);
    }
    static ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
        return libaio_pwrite(fd, buf, count, offset);
    }
    static ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
        return libaio_pwritev(fd, iov, iovcnt, offset);
    }
    static int fsync(int fd) {
        return libaio_fsync(fd);
    }
    static int fdatasync(int fd) {
        return libaio_fsync(fd);
    }
};

struct posixaio {
    static ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
        return posixaio_pread(fd, buf, count, offset);
    }
    static ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
    static ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
        return posixaio_pwrite(fd, buf, count, offset);
    }
    static ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);
    static int fsync(int fd) {
        return posixaio_fsync(fd);
    }
    static int fdatasync(int fd) {
        return posixaio_fdatasync(fd);
    }
};
} // namespace photon
